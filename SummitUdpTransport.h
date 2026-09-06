#pragma once

#include "SummitMovementTransport.h"
#include "ServerCore/Runtime/DatagramTransport.h"

#include <functional>
#include <string_view>

namespace Summit
{
// Summit message admission and handshake over the shared datagram transport.
// Main schedules Poll on the game backend's JobRunner.
class SummitUdpTransport final : public IMovementTransport
{
public:
    using Receiver = ServerCore::Runtime::DatagramTransport::Receiver;
    using Metrics = ServerCore::Runtime::DatagramTransport::Metrics;

    SummitUdpTransport();
    ~SummitUdpTransport() override;
    SummitUdpTransport(const SummitUdpTransport&) = delete;
    SummitUdpTransport& operator=(const SummitUdpTransport&) = delete;
    [[nodiscard]] ServerCore::Core::Status Bind(std::string_view address, std::uint16_t port);
    void Close() noexcept;
    void Poll(const Receiver& receiver) noexcept;
    [[nodiscard]] Metrics SnapshotMetrics() const noexcept;
    [[nodiscard]] std::uint16_t Port() const noexcept override;
    [[nodiscard]] ServerCore::Core::Result<std::string> RegisterSession(
        ServerCore::Session::SessionId id) override;
    void UnregisterSession(ServerCore::Session::SessionId id) noexcept override;
    [[nodiscard]] bool IsReady(ServerCore::Session::SessionId id) const noexcept override;
    [[nodiscard]] ServerCore::Core::Status SendState(ServerCore::Session::SessionId id,
        const ServerCore::Protocol::PreparedMessage& message) noexcept override;

private:
    ServerCore::Runtime::DatagramTransport mTransport;
};
}
