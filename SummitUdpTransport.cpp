#include "SummitUdpTransport.h"

#include <new>
#include <utility>

namespace Summit
{
namespace
{
namespace Codec = ServerCore::Protocol::DatagramCodec;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Result;
using ServerCore::Core::Status;
using ServerCore::Session::SessionId;
}

SummitUdpTransport::SummitUdpTransport() = default;
SummitUdpTransport::~SummitUdpTransport() = default;

Status SummitUdpTransport::Bind(std::string_view address, std::uint16_t port)
{
    return mTransport.Bind(address, port);
}

void SummitUdpTransport::Close() noexcept
{
    mTransport.Close();
}

std::uint16_t SummitUdpTransport::Port() const noexcept
{
    return mTransport.Port();
}

Result<std::string> SummitUdpTransport::RegisterSession(SessionId id)
{
    auto token = mTransport.RegisterSession(id);
    if (!token.IsOk())
        return Result<std::string>::FromStatus(std::move(token).TakeStatus());
    try
    {
        return Result<std::string>::FromValue(Codec::TokenToHex(token.Value()));
    }
    catch (const std::bad_alloc&)
    {
        mTransport.UnregisterSession(id);
        return Result<std::string>::FromStatus(Status::AllocationFailure());
    }
}

void SummitUdpTransport::UnregisterSession(SessionId id) noexcept
{
    mTransport.UnregisterSession(id);
}

bool SummitUdpTransport::IsReady(SessionId id) const noexcept
{
    return mTransport.IsReady(id);
}

Status SummitUdpTransport::SendState(SessionId id,
    const ServerCore::Protocol::PreparedMessage& message) noexcept
{
    auto status = mTransport.Send(id, message.Bytes());
    // UDP path errors must not terminate the reliable game session.
    if (status.Code() == ErrorCode::PlatformError)
        return Status::FailWithoutMessage(ErrorCode::WouldBlock);
    return status;
}

void SummitUdpTransport::Poll(const Receiver& receiver) noexcept
{
    mTransport.Poll(
        [](SessionId, const ServerCore::Protocol::Message& message)
        {
            return message.Body() && message.Body()->IsObject() &&
                (message.Type() == "UdpHello" || message.Type() == "PlayerState");
        },
        [this, &receiver](SessionId id, const ServerCore::Protocol::Message& message)
        {
            if (message.Type() == "UdpHello")
            {
                constexpr std::string_view Ready = R"({"type":"UdpReady","body":{}})";
                (void)mTransport.Send(id, std::as_bytes(std::span(Ready.data(), Ready.size())));
            }
            else if (receiver)
            {
                receiver(id, message);
            }
        });
}

SummitUdpTransport::Metrics SummitUdpTransport::SnapshotMetrics() const noexcept
{
    return mTransport.SnapshotMetrics();
}
}
