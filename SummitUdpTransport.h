#pragma once

#include "SummitMovementTransport.h"

#include <functional>
#include <memory>
#include <string_view>

namespace Summit
{
// A bounded nonblocking UDP pump. Main schedules Poll on the same JobRunner as
// the game backend; no per-client threads or historical movement queues exist.
class SummitUdpTransport final : public IMovementTransport
{
public:
    using Receiver = std::function<void(ServerCore::Session::SessionId,
        const ServerCore::Protocol::Message&)>;
    struct Metrics
    {
        std::uint64_t receivedDatagrams = 0, receivedBytes = 0;
        std::uint64_t sentDatagrams = 0, sentBytes = 0;
        std::uint64_t rejectedDatagrams = 0, sendWouldBlock = 0, socketErrors = 0;
    };

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
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
}
