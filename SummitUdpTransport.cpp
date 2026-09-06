#include "SummitUdpTransport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace Summit
{
namespace
{
namespace Codec = ServerCore::Protocol::DatagramCodec;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCore::Core::Result;
using ServerCore::Session::SessionId;
using ServerCore::Protocol::ParseMessage;

struct TokenHash
{
    std::size_t operator()(const Codec::Token& token) const noexcept
    {
        std::size_t value = 1469598103934665603ull;
        for (const auto byte : token) value = (value ^ std::to_integer<unsigned>(byte)) * 1099511628211ull;
        return value;
    }
};
Status SocketFailure(const char* operation)
{
    return Status::Fail(ErrorCode::PlatformError,
        std::string(operation) + " (Winsock " + std::to_string(::WSAGetLastError()) + ")");
}
}

struct SummitUdpTransport::Impl
{
    struct Peer
    {
        Codec::Token token{};
        sockaddr_in endpoint{};
        std::uint64_t receivedSequence = 0, sentSequence = 0;
        bool ready = false;
    };
    mutable std::mutex mutex;
    SOCKET socket = INVALID_SOCKET;
    bool winsock = false;
    std::uint16_t port = 0;
    std::unordered_map<SessionId, Peer> peers;
    std::unordered_map<Codec::Token, SessionId, TokenHash> tokens;
    Metrics metrics;

    Status Send(Peer& peer, const std::span<const std::byte> payload) noexcept
    {
        if (!peer.ready || socket == INVALID_SOCKET) return Status::FailWithoutMessage(ErrorCode::WouldBlock);
        if (peer.sentSequence == std::numeric_limits<std::uint64_t>::max())
            return Status::FailWithoutMessage(ErrorCode::Closed);
        std::array<std::byte, Codec::MaximumDatagramBytes> packet{};
        const auto count = Codec::Encode(packet, peer.token, peer.sentSequence + 1, payload);
        if (count == 0) return Status::FailWithoutMessage(ErrorCode::TooLarge);
        const int sent = ::sendto(socket, reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(count), 0, reinterpret_cast<const sockaddr*>(&peer.endpoint), sizeof(peer.endpoint));
        if (sent == SOCKET_ERROR)
        {
            const int error = ::WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAENOBUFS)
            {
                ++metrics.sendWouldBlock;
                return Status::FailWithoutMessage(ErrorCode::WouldBlock);
            }
            ++metrics.socketErrors;
            // A bad/unreachable UDP path must not tear down the reliable session.
            return Status::FailWithoutMessage(ErrorCode::WouldBlock);
        }
        if (sent != static_cast<int>(count)) return Status::FailWithoutMessage(ErrorCode::WouldBlock);
        ++peer.sentSequence;
        ++metrics.sentDatagrams;
        metrics.sentBytes += count;
        return Status::Ok();
    }
};

SummitUdpTransport::SummitUdpTransport() : mImpl(std::make_unique<Impl>()) {}
SummitUdpTransport::~SummitUdpTransport() { Close(); }

Status SummitUdpTransport::Bind(const std::string_view address, const std::uint16_t port)
{
    const std::lock_guard guard(mImpl->mutex);
    if (mImpl->socket != INVALID_SOCKET) return Status::FailWithoutMessage(ErrorCode::AlreadyExists);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    const std::string addressText(address);
    if (::InetPtonA(AF_INET, addressText.c_str(), &endpoint.sin_addr) != 1)
        return Status::Fail(ErrorCode::InvalidArgument, "UDP listener needs an IPv4 address");
    if (!mImpl->winsock)
    {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return Status::Fail(ErrorCode::PlatformError, "UDP Winsock startup failed");
        mImpl->winsock = true;
    }
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) return SocketFailure("UDP socket");
    const auto fail = [&](const char* operation) {
        auto status = SocketFailure(operation);
        ::closesocket(socket);
        return status;
    };
    // Exclusive bind prevents a second server silently sharing this port. Buffers
    // are bounded kernel storage, not one allocation for every possible session.
    const BOOL exclusive = TRUE;
    const int receiveBytes = 4 * 1024 * 1024;
    const int sendBytes = 1024 * 1024;
    u_long nonblocking = 1;
    if (::setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR ||
        ::setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
            reinterpret_cast<const char*>(&receiveBytes), sizeof(receiveBytes)) == SOCKET_ERROR ||
        ::setsockopt(socket, SOL_SOCKET, SO_SNDBUF,
            reinterpret_cast<const char*>(&sendBytes), sizeof(sendBytes)) == SOCKET_ERROR ||
        ::ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR)
        return fail("UDP socket configuration");
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR)
        return fail("UDP bind");
    int length = sizeof(endpoint);
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&endpoint), &length) == SOCKET_ERROR)
        return fail("UDP getsockname");
    mImpl->socket = socket;
    mImpl->port = ntohs(endpoint.sin_port);
    return Status::Ok();
}

void SummitUdpTransport::Close() noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    if (mImpl->socket != INVALID_SOCKET) ::closesocket(mImpl->socket);
    mImpl->socket = INVALID_SOCKET;
    mImpl->port = 0;
    mImpl->peers.clear();
    mImpl->tokens.clear();
    if (mImpl->winsock) ::WSACleanup();
    mImpl->winsock = false;
}

std::uint16_t SummitUdpTransport::Port() const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    return mImpl->port;
}

Result<std::string> SummitUdpTransport::RegisterSession(const SessionId id)
{
    try
    {
        const std::lock_guard guard(mImpl->mutex);
        if (mImpl->socket == INVALID_SOCKET) return Result<std::string>::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
        if (id == SessionId::Invalid || mImpl->peers.contains(id))
            return Result<std::string>::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
        Impl::Peer peer;
        do
        {
            if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(peer.token.data()),
                static_cast<ULONG>(peer.token.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                return Result<std::string>::FromStatus(Status::FailWithoutMessage(ErrorCode::PlatformError));
        } while (mImpl->tokens.contains(peer.token));
        std::string text = Codec::TokenToHex(peer.token);
        mImpl->peers.emplace(id, peer);
        try { mImpl->tokens.emplace(peer.token, id); }
        catch (...) { mImpl->peers.erase(id); throw; }
        return Result<std::string>::FromValue(std::move(text));
    }
    catch (const std::bad_alloc&) { return Result<std::string>::FromStatus(Status::AllocationFailure()); }
}

void SummitUdpTransport::UnregisterSession(const SessionId id) noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    const auto found = mImpl->peers.find(id);
    if (found == mImpl->peers.end()) return;
    mImpl->tokens.erase(found->second.token);
    mImpl->peers.erase(found);
}

bool SummitUdpTransport::IsReady(const SessionId id) const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    const auto found = mImpl->peers.find(id);
    return found != mImpl->peers.end() && found->second.ready;
}

Status SummitUdpTransport::SendState(const SessionId id,
    const ServerCore::Protocol::PreparedMessage& message) noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    const auto found = mImpl->peers.find(id);
    if (found == mImpl->peers.end()) return Status::FailWithoutMessage(ErrorCode::Closed);
    return mImpl->Send(found->second, message.Bytes());
}

void SummitUdpTransport::Poll(const Receiver& receiver) noexcept
{
    // Bound work even under unsolicited traffic so UDP cannot monopolize the
    // JobRunner. Unknown tokens get no response and are rejected before JSON work.
    constexpr std::size_t MaximumPacketsPerPoll = 4096;
    constexpr std::size_t MaximumBytesPerPoll = 1024 * 1024;
    std::size_t consumed = 0;
    for (std::size_t index = 0; index < MaximumPacketsPerPoll && consumed < MaximumBytesPerPoll; ++index)
    {
        try
        {
            std::array<std::byte, Codec::MaximumDatagramBytes + 1> bytes{};
            sockaddr_in source{};
            int length = sizeof(source);
            std::unique_lock guard(mImpl->mutex);
            if (mImpl->socket == INVALID_SOCKET) return;
            const int count = ::recvfrom(mImpl->socket, reinterpret_cast<char*>(bytes.data()),
                static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&source), &length);
            if (count == SOCKET_ERROR)
            {
                const int error = ::WSAGetLastError();
                if (error == WSAEWOULDBLOCK) return;
                if (error == WSAEMSGSIZE) { ++mImpl->metrics.rejectedDatagrams; consumed += bytes.size(); continue; }
                ++mImpl->metrics.socketErrors;
                // A closed peer can produce an ICMP reset on this shared socket.
                // It must not delay every other peer until the next 10ms pump.
                if (error == WSAECONNRESET || error == WSAECONNREFUSED) continue;
                return;
            }
            ++mImpl->metrics.receivedDatagrams;
            mImpl->metrics.receivedBytes += static_cast<std::size_t>(count);
            consumed += static_cast<std::size_t>(count);
            const auto packet = Codec::Decode(std::span(bytes).first(static_cast<std::size_t>(count)));
            if (!packet || source.sin_family != AF_INET)
            { ++mImpl->metrics.rejectedDatagrams; continue; }
            const auto token = mImpl->tokens.find(packet->token);
            if (token == mImpl->tokens.end()) { ++mImpl->metrics.rejectedDatagrams; continue; }
            const SessionId id = token->second;
            auto& peer = mImpl->peers.at(id);
            if (packet->sequence <= peer.receivedSequence)
            { ++mImpl->metrics.rejectedDatagrams; continue; }
            auto message = ParseMessage(packet->payload);
            if (!message.IsOk() || !message.Value().Body() || !message.Value().Body()->TryObject() ||
                (message.Value().Type() != "UdpHello" && message.Value().Type() != "PlayerState"))
            { ++mImpl->metrics.rejectedDatagrams; continue; }
            // Only a valid, newer packet may establish or replace the return
            // endpoint (NAT rebinding). Replayed traffic cannot steal that binding.
            peer.receivedSequence = packet->sequence;
            peer.endpoint = source;
            peer.ready = true;
            if (message.Value().Type() == "UdpHello")
            {
                constexpr std::string_view Ready = R"({"type":"UdpReady","body":{}})";
                (void)mImpl->Send(peer, std::as_bytes(std::span(Ready.data(), Ready.size())));
                continue;
            }
            // Do not hold the transport lock while backend close/SendState paths
            // can call us again. All packet bytes and the parsed message still live.
            guard.unlock();
            receiver(id, message.Value());
        }
        catch (...)
        {
            const std::lock_guard guard(mImpl->mutex);
            ++mImpl->metrics.rejectedDatagrams;
            return;
        }
    }
}

SummitUdpTransport::Metrics SummitUdpTransport::SnapshotMetrics() const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    return mImpl->metrics;
}
}
