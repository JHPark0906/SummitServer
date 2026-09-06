#pragma once

#include "ServerCore/Protocol/DatagramCodec.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Session/Session.h"

#include <cstdint>
#include <string>

namespace Summit
{
// Application-specific unreliable movement channel. TCP Session continues to own
// identity and reliable control. Registration lasts exactly that joined session.
class IMovementTransport
{
public:
    virtual ~IMovementTransport() = default;
    [[nodiscard]] virtual std::uint16_t Port() const noexcept = 0;
    [[nodiscard]] virtual ServerCore::Core::Result<std::string> RegisterSession(
        ServerCore::Session::SessionId id) = 0;
    virtual void UnregisterSession(ServerCore::Session::SessionId id) noexcept = 0;
    [[nodiscard]] virtual bool IsReady(ServerCore::Session::SessionId id) const noexcept = 0;
    // Success means the OS accepted a datagram, not that a peer received it.
    [[nodiscard]] virtual ServerCore::Core::Status SendState(
        ServerCore::Session::SessionId id,
        const ServerCore::Protocol::PreparedMessage& message) noexcept = 0;
};
}
