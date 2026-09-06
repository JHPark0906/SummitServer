#include "SummitServerBackend.h"
#include "SummitMovementTransport.h"

#include "ServerCore/Core/Clock.h"
#include "ServerCore/Core/Assert.h"
#include "ServerCore/Core/Logging.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <new>
#include <stdexcept>
#include <span>
#include <string_view>
#include <utility>

namespace Summit
{
namespace
{
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCore::Protocol::JsonValue;
using ServerCore::Protocol::PreparedJsonValue;
using ServerCore::Protocol::PreparedMessage;
using ServerCore::Session::Session;
using ServerCore::Session::SessionId;
using ServerCore::Session::SessionState;

constexpr std::uint64_t MaximumSelectableCharacter = 4;
constexpr std::uint64_t NicknameCharacter = 5;

template <typename BuildFields>
void LogGameplayEvent(const std::string_view event, BuildFields&& buildFields) noexcept
{
    // 필드 복사와 JSON 생성도 실패할 수 있으므로 호출자 밖이 아니라 이 경계 안에서 한다.
    // Dump가 닉네임/채팅의 따옴표·역슬래시·제어문자를 escape해 로그 한 줄을 보존한다.
    try
    {
        JsonValue::Object fields = buildFields();
        fields.emplace("event", JsonValue(std::string(event)));
        const auto line = JsonValue(std::move(fields)).Dump();
        if (line.IsOk())
        {
            ServerCore::Core::GetGlobalLogger().Write(ServerCore::Core::LogLevel::Info, line.Value());
        }
    }
    catch (...)
    {
        // 관측 실패는 가입/퇴장/채팅의 성공 여부나 롤백 경로에 영향을 주지 않는다.
    }
}

[[nodiscard]] std::size_t ValidateCapacity(
    const std::size_t players, const std::size_t connections)
{
    if (players == 0 || connections == 0 || players > connections ||
        connections > SummitServerBackend::MaximumSessionCapacity)
    {
        throw std::invalid_argument("Summit capacity must satisfy 1 <= players <= connections <= 65536");
    }
    return connections;
}

[[nodiscard]] SummitServerBackend::SchedulerOptions ValidateSchedulerOptions(
    const SummitServerBackend::SchedulerOptions options)
{
    for (const std::size_t bytes : { options.perClientStateBytesPerTick, options.globalStateBytesPerTick,
             options.perClientControlBytesPerTick, options.globalControlBytesPerTick })
        if (bytes < SummitServerBackend::MinimumSchedulerBudget || bytes > SummitServerBackend::MaximumSchedulerBudget)
            throw std::invalid_argument("Scheduler budgets must be within 1024..16777216 bytes");
    return options;
}

[[nodiscard]] bool IsWithinView(const double viewerX, const double viewerY,
    const double targetX, const double targetY, const bool alreadyVisible) noexcept
{
    return std::abs(viewerX - targetX) <= (alreadyVisible ? SummitServerBackend::ExitHalfWidth : SummitServerBackend::EnterHalfWidth) &&
        std::abs(viewerY - targetY) <= (alreadyVisible ? SummitServerBackend::ExitHalfHeight : SummitServerBackend::EnterHalfHeight);
}

// 각 작업의 실행 소유권이 이 공유 scratch 저장소를 보호한다. 예외로 빠져나가도 외부
// Session 수명을 불필요하게 연장하지 않으며, 큰 용량을 스레드 스택에 올리지 않는다.
struct SessionSnapshot
{
    std::vector<std::shared_ptr<Session>>& sessions;
    std::size_t count = 0;

    ~SessionSnapshot()
    {
        for (std::size_t index = 0; index < count; ++index) sessions[index].reset();
    }
};

[[nodiscard]] std::uint64_t NumericSessionId(const SessionId id) noexcept
{
    return static_cast<std::uint64_t>(id);
}

[[nodiscard]] std::string WireSessionId(const SessionId id)
{
    // JSON numbers lose integer precision in many clients beyond 2^53. A
    // decimal string preserves the full monotonic uint64 SessionId contract.
    return std::to_string(NumericSessionId(id));
}

[[nodiscard]] Status InvalidFormat(std::string message)
{
    return Status::Fail(ErrorCode::InvalidFormat, std::move(message));
}

[[nodiscard]] bool ReadUnsignedInteger(const JsonValue* const value,
    std::uint64_t& output) noexcept
{
    if (value == nullptr)
    {
        return false;
    }
    if (const std::uint64_t* const integer = value->TryUInt64())
    {
        output = *integer;
        return true;
    }
    if (const std::int64_t* const integer = value->TryInt64())
    {
        if (*integer < 0)
        {
            return false;
        }
        output = static_cast<std::uint64_t>(*integer);
        return true;
    }
    if (const double* const number = value->TryNumber())
    {
        constexpr double MaximumExactInteger = 9007199254740991.0;
        if (std::isfinite(*number) && *number >= 0.0 && *number <= MaximumExactInteger &&
            std::floor(*number) == *number)
        {
            output = static_cast<std::uint64_t>(*number);
            return true;
        }
    }
    return false;
}

[[nodiscard]] const JsonValue* FindFiniteFloatNumber(
    const JsonValue& body, const std::string_view key) noexcept
{
    const JsonValue* const value = body.Find(key);
    if (value == nullptr)
    {
        return nullptr;
    }

    const double* const number = value->TryNumber();
    if (number == nullptr || !std::isfinite(*number))
    {
        return nullptr;
    }

    const double maximum = static_cast<double>((std::numeric_limits<float>::max)());
    if (*number < -maximum || *number > maximum)
    {
        return nullptr;
    }
    return value;
}

[[nodiscard]] bool ReadMovementSequence(const JsonValue* value, std::uint64_t& sequence) noexcept
{
    if (ReadUnsignedInteger(value, sequence)) return true;
    const std::string* text = value ? value->TryString() : nullptr;
    if (!text || text->empty() || text->size() > 20 ||
        (text->size() > 1 && text->front() == '0')) return false;
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), sequence);
    return parsed.ec == std::errc{} && parsed.ptr == text->data() + text->size();
}

[[nodiscard]] Status BackendUnavailable()
{
    return Status::Fail(ErrorCode::Closed, std::string{});
}

// UTF-8 형식은 ParseMessage가 검사한다. 닉네임은 trim이나 Unicode 정규화를 하지 않아
// 중복 판정과 특수 캐릭터 선택이 모두 클라이언트가 보낸 동일한 문자열을 기준으로 한다.
[[nodiscard]] bool IsNameValid(const std::string* name)
{
    return name && !name->empty() && name->size() <= SummitServerBackend::MaximumNameBytes &&
        name->find_first_not_of(' ') != std::string::npos &&
        std::none_of(name->begin(), name->end(), [](unsigned char ch) { return ch < 32 || ch == 127; });
}

[[nodiscard]] bool IsChatTextValid(const std::string* text) noexcept
{
    // ParseMessage already validates UTF-8 before dispatch. Preserve authored spaces and
    // Unicode exactly; text length is decoded UTF-8 bytes, not JSON escape bytes or glyphs.
    return text && !text->empty() && text->size() <= SummitServerBackend::MaximumChatBytes &&
        text->find_first_not_of(' ') != std::string::npos &&
        std::none_of(text->begin(), text->end(), [](const unsigned char ch) {
            return ch < 32 || ch == 127;
        });
}

[[nodiscard]] bool ReadProfileCharacter(const std::string_view name,
    const JsonValue* const requestedValue, std::uint64_t& character) noexcept
{
    // Clients retain their ordinary A-E selection even while a nickname selects tdw.
    // Never accept tdw directly: only the server applies this exact UTF-8 name rule.
    if (!ReadUnsignedInteger(requestedValue, character) || character > MaximumSelectableCharacter)
    {
        return false;
    }
    if (name == "상여자" || name == "심심이심셔")
    {
        character = NicknameCharacter;
    }
    return true;
}
}

SummitServerBackend::SummitServerBackend(
    const std::size_t playerCapacity, const std::size_t connectionCapacity)
    : SummitServerBackend(playerCapacity, connectionCapacity, SchedulerOptions{})
{
}

SummitServerBackend::SummitServerBackend(const std::size_t playerCapacity,
    const std::size_t connectionCapacity, const SchedulerOptions schedulerOptions)
    : mPlayerCapacity(playerCapacity)
    , mConnectionCapacity(connectionCapacity)
    , mSchedulerOptions(ValidateSchedulerOptions(schedulerOptions))
    , mDeferredCloses(ValidateCapacity(playerCapacity, connectionCapacity))
    , mDeferredCloseBuckets(connectionCapacity, NoDeferredClose)
    , mRecipients(playerCapacity)
    , mExpiredSessions(connectionCapacity)
{
}

std::size_t SummitServerBackend::SessionIdHash::operator()(const SessionId id) const noexcept
{
    return std::hash<std::uint64_t>{}(NumericSessionId(id));
}

void SummitServerBackend::SetMovementTransport(std::shared_ptr<IMovementTransport> transport)
{
    const Operation operation(*this);
    if (!mPlayers.empty() || !mPendingJoins.empty())
        throw std::logic_error("movement transport must be configured before sessions open");
    mMovementTransport = std::move(transport);
}

void SummitServerBackend::ReceiveMovement(const SessionId id,
    const ServerCore::Protocol::Message& message) noexcept
{
    try
    {
        const Operation operation(*this);
        const auto found = mPlayers.find(id);
        if (!mMovementTransport || found == mPlayers.end() || !found->second.udpMovement ||
            !mMovementTransport->IsReady(id) || message.Type() != "PlayerState" ||
            !message.Body() || !message.Body()->TryObject() ||
            message.RawBodySize() > ServerCore::Protocol::DatagramCodec::MaximumPayloadBytes) return;
        const auto session = found->second.session.lock();
        if (!session || session->State() != SessionState::Authenticated) return;
        (void)HandlePlayerStateImpl(session, message, true);
    }
    catch (...)
    {
        // UDP의 단일 입력 준비 실패도 마지막 정상 상태를 유지한다. 다음 새 sequence를
        // 받으면 다시 갱신할 수 있으며 신뢰 제어와 TCP 세션의 수명은 바꾸지 않는다.
    }
}

SummitServerBackend::Operation::Operation(SummitServerBackend& backend)
    : mBackend(backend)
{
    mBackend.BeginOperation();
}

SummitServerBackend::Operation::~Operation()
{
    mBackend.FinishOperation();
}

void SummitServerBackend::BeginOperation()
{
    std::unique_lock lock(mOperationMutex);
    mOperationFinished.wait(lock, [this]() { return !mOperationActive; });
    mOperationActive = true;
}

// 종료 통지를 처리하는 동안에도 실행 소유권을 유지한다. PlayerLeft 송신에서 다시 발생한
// 닫힘까지 모두 비운 뒤에만 다음 Join/Profile/Chat 작업이 명단을 바꿀 수 있다.
void SummitServerBackend::FinishOperation() noexcept
{
    for (;;)
    {
        SessionId closedId;
        ErrorCode reason;
        {
            const std::lock_guard lock(mOperationMutex);
            if (mDeferredCloseCount == 0)
            {
                // Queue inspection and releasing ownership share the lock with
                // OnSessionClosed: an arriving close is either drained here or
                // becomes the next owner, never stranded between the two.
                mOperationActive = false;
                mOperationFinished.notify_all();
                return;
            }
            const DeferredClose& closing = mDeferredCloses[mDeferredCloseBegin];
            closedId = closing.id;
            reason = closing.reason;
            std::size_t* link = &mDeferredCloseBuckets[SessionIdHash{}(closedId) %
                mDeferredCloseBuckets.size()];
            while (*link != mDeferredCloseBegin)
            {
                link = &mDeferredCloses[*link].nextInBucket;
            }
            *link = closing.nextInBucket;
            mDeferredCloseBegin = (mDeferredCloseBegin + 1) % mDeferredCloses.size();
            --mDeferredCloseCount;
        }
        HandleSessionClosed(closedId, reason);
    }
}

Status SummitServerBackend::RegisterHandlers(ServerCore::Dispatch::Dispatcher& dispatcher)
{
    const std::weak_ptr<SummitServerBackend> weakSelf = weak_from_this();
    if (weakSelf.expired())
    {
        return Status::Fail(ErrorCode::InvalidArgument,
            "SummitServerBackend must be shared before handlers are registered");
    }

    Status registered = dispatcher.Register("Join",
        [weakSelf](const std::shared_ptr<Session>& session,
            const ServerCore::Protocol::Message& message) {
            const std::shared_ptr<SummitServerBackend> self = weakSelf.lock();
            return self ? self->HandleJoin(session, message) : BackendUnavailable();
        });
    if (!registered.IsOk())
    {
        return registered;
    }

    registered = dispatcher.Register("PlayerState",
        [weakSelf](const std::shared_ptr<Session>& session,
            const ServerCore::Protocol::Message& message) {
            const std::shared_ptr<SummitServerBackend> self = weakSelf.lock();
            return self ? self->HandlePlayerState(session, message) : BackendUnavailable();
        });
    if (!registered.IsOk())
    {
        return registered;
    }
    registered = dispatcher.Register("SetProfile",
        [weakSelf](const std::shared_ptr<Session>& session,
            const ServerCore::Protocol::Message& message) {
            const auto self = weakSelf.lock();
            return self ? self->HandleSetProfile(session, message) : BackendUnavailable();
        });
    if (!registered.IsOk())
    {
        return registered;
    }
    registered = dispatcher.Register("Chat",
        [weakSelf](const std::shared_ptr<Session>& session,
            const ServerCore::Protocol::Message& message) {
            const auto self = weakSelf.lock();
            return self ? self->HandleChat(session, message) : BackendUnavailable();
        });
    if (!registered.IsOk()) return registered;
    registered = dispatcher.Register("DirectoryAck",
        [weakSelf](const std::shared_ptr<Session>& session,
            const ServerCore::Protocol::Message& message) {
            const auto self = weakSelf.lock();
            return self ? self->HandleDirectoryAck(session, message) : BackendUnavailable();
        });
    if (!registered.IsOk()) return registered;
    return dispatcher.Register("Heartbeat",
        [weakSelf](const std::shared_ptr<Session>& session,
            const ServerCore::Protocol::Message& message) {
            const auto self = weakSelf.lock();
            return self ? self->HandleHeartbeat(session, message) : BackendUnavailable();
        });
}

void SummitServerBackend::OnSessionOpened(const std::shared_ptr<Session>& session)
{
    if (!session)
    {
        return;
    }

    try
    {
        const Operation operation(*this);
        const auto [position, inserted] = mPendingJoins.emplace(session->Id(),
            PendingJoin{ ServerCore::Core::MillisecondsSinceProcessStart(), session });
        (void)position;
        if (!inserted)
        {
            session->Disconnect(Status::Fail(ErrorCode::InvalidArgument, std::string{}));
        }
    }
    catch (...)
    {
        session->Disconnect(Status::Fail(ErrorCode::PlatformError, std::string{}));
    }
}

void SummitServerBackend::OnSessionClosed(const SessionId id, Status reason) noexcept
{
    {
        const std::lock_guard lock(mOperationMutex);
        const std::size_t bucket = SessionIdHash{}(id) % mDeferredCloseBuckets.size();
        for (std::size_t index = mDeferredCloseBuckets[bucket]; index != NoDeferredClose;
            index = mDeferredCloses[index].nextInBucket)
        {
            if (mDeferredCloses[index].id == id)
            {
                return;
            }
        }
        // Core emits each opened session's close exactly once. While an
        // operation owns this gate, JobRunner cannot open replacement sessions:
        // even though Core frees its slot before NotifyClosed, a new OnOpened
        // must acquire the same gate. Thus at most mConnectionCapacity distinct
        // close IDs can be pending. Storage stays available during OOM fallback.
        SERVERCORE_ASSERT(mDeferredCloseCount < mDeferredCloses.size(),
            "SummitServer exceeded its configured concurrent-session close bound");
        const std::size_t tail =
            (mDeferredCloseBegin + mDeferredCloseCount) % mDeferredCloses.size();
        mDeferredCloses[tail] = DeferredClose{ id, mDeferredCloseBuckets[bucket], reason.Code() };
        mDeferredCloseBuckets[bucket] = tail;
        ++mDeferredCloseCount;
        if (mOperationActive)
        {
            return;
        }
        mOperationActive = true;
    }
    FinishOperation();
}

void SummitServerBackend::HandleSessionClosed(const SessionId id, const ErrorCode reason) noexcept
{
    try
    {
        mPendingJoins.erase(id);
        if (mMovementTransport) mMovementTransport->UnregisterSession(id);
        const auto found = mPlayers.find(id);
        if (found == mPlayers.end())
        {
            return;
        }

        const Player leaving = std::move(found->second);
        mPlayers.erase(found);
        mNames.erase(leaving.name);
        if (leaving.state)
        {
            const auto cell = mCells.find(StateCell(*leaving.state));
            if (cell != mCells.end())
            {
                cell->second.erase(id);
                if (cell->second.empty()) mCells.erase(cell);
            }
        }
        mVisibleEdges -= leaving.visible.size();
        for (auto& [otherId, player] : mPlayers)
        {
            (void)otherId;
            mVisibleEdges -= player.visible.erase(id);
        }
        LogGameplayEvent("player_left", [&]() {
            JsonValue::Object fields;
            fields.emplace("id", JsonValue(WireSessionId(id)));
            fields.emplace("name", JsonValue(leaving.name));
            fields.emplace("character", JsonValue(leaving.character));
            fields.emplace("reasonCode", JsonValue(static_cast<int>(reason)));
            return fields;
        });
        JsonValue::Object fields;
        fields.emplace("id", JsonValue(WireSessionId(id)));
        Broadcast("PlayerLeft", JsonValue(std::move(fields)), id, true);
        PublishMembershipNotice(false, id, leaving.name);
    }
    catch (...)
    {
        // A leave that cannot even be constructed has the same consistency
        // consequence as a failed reliable Send. With no roster resync, keep
        // no connected receiver that permanently retains the removed player.
        // Snapshot without allocating; Disconnect may queue another close.
        SessionSnapshot recipients{ mRecipients };
        for (const auto& [playerId, player] : mPlayers)
        {
            (void)playerId;
            if (auto recipient = player.session.lock())
            {
                recipients.sessions[recipients.count++] = std::move(recipient);
            }
        }
        for (std::size_t index = 0; index < recipients.count; ++index)
        {
            recipients.sessions[index]->Disconnect(Status::FailWithoutMessage(ErrorCode::PlatformError));
        }
    }
}

void SummitServerBackend::ExpireUnjoinedSessions(const std::uint64_t nowMilliseconds) noexcept
{
    try
    {
        const Operation operation(*this);
        SessionSnapshot expiredSessions{ mExpiredSessions };
        for (auto iterator = mPendingJoins.begin(); iterator != mPendingJoins.end();)
        {
            const auto current = iterator++;
            if (nowMilliseconds - current->second.openedAtMilliseconds < JoinTimeoutMilliseconds)
            {
                continue;
            }

            if (expiredSessions.count < expiredSessions.sessions.size())
            {
                if (std::shared_ptr<Session> session = current->second.session.lock())
                {
                    expiredSessions.sessions[expiredSessions.count++] = std::move(session);
                }
            }
            mPendingJoins.erase(current);
        }

        for (std::size_t index = 0; index < expiredSessions.count; ++index)
        {
            expiredSessions.sessions[index]->Disconnect(
                Status::Fail(ErrorCode::Timeout, std::string{}));
        }
    }
    catch (...)
    {
        // The timeout callback shares the message-handler no-throw boundary.
        // A later periodic pass can retry any entries not reached this time.
    }
}

Status SummitServerBackend::HandleJoin(
    const std::shared_ptr<Session>& session,
    const ServerCore::Protocol::Message& message) noexcept
{
    try
    {
        const Operation operation(*this);
        return HandleJoinImpl(session, message);
    }
    catch (...)
    {
        // This path commonly means allocation failure. Do not allocate again
        // while translating it to ServerCore's value-based error contract.
        if (session)
        {
            session->Disconnect(Status::Fail(ErrorCode::PlatformError, std::string{}));
        }
        return Status::Fail(ErrorCode::PlatformError, std::string{});
    }
}

Status SummitServerBackend::HandlePlayerState(
    const std::shared_ptr<Session>& session,
    const ServerCore::Protocol::Message& message) noexcept
{
    try
    {
        const Operation operation(*this);
        return HandlePlayerStateImpl(session, message);
    }
    catch (...)
    {
        // Keep the exception conversion itself allocation-free; throwing from
        // this noexcept handler would terminate the entire server process.
        if (session)
        {
            session->Disconnect(Status::Fail(ErrorCode::PlatformError, std::string{}));
        }
        return Status::Fail(ErrorCode::PlatformError, std::string{});
    }
}

Status SummitServerBackend::HandleChat(
    const std::shared_ptr<Session>& session,
    const ServerCore::Protocol::Message& message) noexcept
{
    try
    {
        const Operation operation(*this);
        return HandleChatImpl(session, message);
    }
    catch (...)
    {
        if (session)
        {
            session->Disconnect(Status::FailWithoutMessage(ErrorCode::PlatformError));
        }
        return Status::FailWithoutMessage(ErrorCode::PlatformError);
    }
}

Status SummitServerBackend::HandleJoinImpl(
    const std::shared_ptr<Session>& session,
    const ServerCore::Protocol::Message& message)
{
    if (!session)
    {
        return Status::Fail(ErrorCode::InvalidArgument, "Join requires a session");
    }
    if (session->State() != SessionState::Connected)
    {
        if (session->State() == SessionState::Closing || session->State() == SessionState::Closed)
        {
            return Status::Fail(ErrorCode::Closed, "Join arrived for a closing session");
        }
        return SendJoinRejected(session, "protocol_violation", message.Sequence(), true);
    }

    const JsonValue& body = *message.Body();
    std::uint64_t schemaVersion = 0;
    if (!ReadUnsignedInteger(body.Find("schemaVersion"), schemaVersion) ||
        schemaVersion != SchemaVersion)
    {
        return SendJoinRejected(session, "schema_mismatch", message.Sequence(), true);
    }
    const JsonValue* movementValue = body.Find("movementTransport");
    const std::string* movement = movementValue ? movementValue->TryString() : nullptr;
    if (movementValue && (!movement || *movement != "udp"))
        return SendJoinRejected(session, "protocol_violation", message.Sequence(), true);
    const bool useDatagrams = movement && mMovementTransport;

    const JsonValue* const nameValue = body.Find("name");
    const std::string* const name = nameValue ? nameValue->TryString() : nullptr;
    if (!IsNameValid(name))
    {
        return SendJoinRejected(session, "name_invalid", message.Sequence(), false);
    }
    std::uint64_t character = 0;
    if (!ReadProfileCharacter(*name, body.Find("c"), character))
    {
        return SendJoinRejected(session, "character_invalid", message.Sequence(), false);
    }

    if (mNames.contains(*name))
    {
        return SendJoinRejected(session, "name_duplicate", message.Sequence(), false);
    }
    if (mPlayers.size() >= mPlayerCapacity)
    {
        return SendJoinRejected(session, "server_full", message.Sequence(), true);
    }

    const SessionId id = session->Id();
    const auto [insertedAt, inserted] =
        mPlayers.emplace(id, Player{ *name, std::weak_ptr<Session>(session), character, std::nullopt });
    if (!inserted)
    {
        return SendJoinRejected(session, "protocol_violation", message.Sequence(), true);
    }

    // 승인 메시지를 준비하거나 큐에 넣지 못하면 아직 공개하지 않은 가입을 되돌린다.
    // Operation이 닫힘 통지를 미루므로 실패한 가입에 대한 유령 PlayerLeft도 나가지 않는다.
    struct JoinRollback
    {
        decltype(mPlayers)& players;
        decltype(mPlayers)::iterator player;
        decltype(mNames)& names;
        IMovementTransport* transport;
        bool committed = false;

        ~JoinRollback()
        {
            if (!committed)
            {
                if (transport) transport->UnregisterSession(player->first);
                names.erase(player->second.name);
                players.erase(player);
            }
        }
    } rollback{ mPlayers, insertedAt, mNames, useDatagrams ? mMovementTransport.get() : nullptr };
    mNames.emplace(*name, id);
    insertedAt->second.joinedAtMilliseconds = ServerCore::Core::MillisecondsSinceProcessStart();
    insertedAt->second.directoryThroughId = mPlayers.rbegin()->first;
    std::string movementToken;
    if (useDatagrams)
    {
        auto registered = mMovementTransport->RegisterSession(id);
        if (!registered.IsOk())
        {
            const Status failed = std::move(registered).TakeStatus();
            session->Disconnect(failed);
            return failed;
        }
        movementToken = std::move(registered.Value());
        insertedAt->second.udpMovement = true;
    }

    const Status authenticated = session->MarkAuthenticated();
    if (!authenticated.IsOk())
    {
        session->Disconnect(authenticated);
        return authenticated;
    }
    mPendingJoins.erase(id);

    JsonValue::Object acceptedFields;
    acceptedFields.emplace("schemaVersion", JsonValue(SchemaVersion));
    acceptedFields.emplace("id", JsonValue(WireSessionId(id)));
    acceptedFields.emplace("name", JsonValue(*name));
    acceptedFields.emplace("c", JsonValue(character));
    acceptedFields.emplace("capacity", JsonValue(static_cast<std::uint64_t>(mPlayerCapacity)));
    acceptedFields.emplace("players", JsonValue(JsonValue::Array{}));
    acceptedFields.emplace("directoryThroughId", JsonValue(WireSessionId(insertedAt->second.directoryThroughId)));
    if (useDatagrams)
    {
        JsonValue::Object udp;
        udp.emplace("port", JsonValue(static_cast<std::uint64_t>(mMovementTransport->Port())));
        udp.emplace("token", JsonValue(std::move(movementToken)));
        acceptedFields.emplace("udp", JsonValue(std::move(udp)));
    }
    const JsonValue acceptedBody(std::move(acceptedFields));
    const Status accepted = session->Send(ServerCore::Protocol::MessageFields{
        "JoinAccepted", &acceptedBody, message.Sequence(), nullptr });
    if (!accepted.IsOk())
    {
        session->Disconnect(accepted);
        return accepted;
    }

    // 초기 전원 명단은 ACK 창 하나의 DirectoryPage로 보낸다. 실제 새 가입 delta와
    // 입장 공지는 지금 방송하므로 페이지 중에도 전역 프로필/채팅의 의미는 유지된다.
    JsonValue::Object joinedFields;
    joinedFields.emplace("id", JsonValue(WireSessionId(id)));
    joinedFields.emplace("name", JsonValue(*name));
    joinedFields.emplace("c", JsonValue(character));
    Broadcast("PlayerJoined", JsonValue(std::move(joinedFields)), id, true);
    rollback.committed = true;
    LogGameplayEvent("player_joined", [&]() {
        JsonValue::Object fields;
        fields.emplace("id", JsonValue(WireSessionId(id)));
        fields.emplace("name", JsonValue(*name));
        fields.emplace("character", JsonValue(character));
        return fields;
    });
    // 명단 replay에는 알림이 없다. 가입이 확정된 뒤 자기 자신을 포함해 한 번만 전송한다.
    PublishMembershipNotice(true, id, *name);
    return Status::Ok();
}

Status SummitServerBackend::HandleSetProfile(
    const std::shared_ptr<Session>& session,
    const ServerCore::Protocol::Message& message) noexcept
{
    try
    {
        const Operation operation(*this);
        return HandleSetProfileImpl(session, message);
    }
    catch (...)
    {
        if (session)
        {
            session->Disconnect(Status::Fail(ErrorCode::PlatformError, std::string{}));
        }
        return Status::Fail(ErrorCode::PlatformError, std::string{});
    }
}

Status SummitServerBackend::HandleSetProfileImpl(
    const std::shared_ptr<Session>& session,
    const ServerCore::Protocol::Message& message)
{
    if (!session)
    {
        return Status::Fail(ErrorCode::InvalidArgument, "SetProfile requires a session");
    }
    const auto found = mPlayers.find(session->Id());
    if (session->State() != SessionState::Authenticated || found == mPlayers.end())
    {
        return SendJoinRejected(session, "protocol_violation", message.Sequence(), true);
    }
    const JsonValue& body = *message.Body();
    const JsonValue* nameValue = body.Find("name");
    const std::string* name = nameValue ? nameValue->TryString() : nullptr;
    std::uint64_t character = 0;
    std::string errorCode;
    if (!IsNameValid(name))
    {
        errorCode = "name_invalid";
    }
    else if (!ReadProfileCharacter(*name, body.Find("c"), character))
    {
        errorCode = "character_invalid";
    }
    else
    {
        const auto owner = mNames.find(*name);
        if (owner != mNames.end() && owner->second != session->Id())
        {
            errorCode = "name_duplicate";
        }
    }
    if (!errorCode.empty())
    {
        JsonValue::Object fields;
        fields.emplace("code", JsonValue(std::move(errorCode)));
        const JsonValue error(std::move(fields));
        const Status sent = session->Send(ServerCore::Protocol::MessageFields{
            "ProfileRejected", nullptr, message.Sequence(), &error });
        if (!sent.IsOk()) session->Disconnect(sent);
        return sent;
    }

    if (found->second.state && mStateRevision == (std::numeric_limits<std::uint64_t>::max)())
    {
        session->Disconnect(Status::FailWithoutMessage(ErrorCode::TooLarge));
        return Status::FailWithoutMessage(ErrorCode::TooLarge);
    }
    // 할당이 필요한 응답을 먼저 완성한다. 이후 swap과 캐릭터 대입으로 프로필을 함께 확정해
    // 응답 준비 실패가 이름만 바뀐 상태를 남기지 않게 한다.
    std::string approvedName = *name;
    JsonValue::Object changedFields;
    changedFields.emplace("id", JsonValue(WireSessionId(session->Id())));
    changedFields.emplace("name", JsonValue(approvedName));
    changedFields.emplace("c", JsonValue(character));
    const JsonValue changed(std::move(changedFields));
    // 새 이름의 인덱스 할당도 commit 전에 끝낸다. 기존 이름과 같으면 기존 노드를 유지한다.
    if (approvedName != found->second.name)
    {
        mNames.emplace(approvedName, session->Id());
        mNames.erase(found->second.name);
    }
    found->second.name.swap(approvedName);
    found->second.character = character;
    if (found->second.state)
    {
        // q는 클라이언트 입력 순서다. 승인 프로필 변경은 서버 상태 세대만 증가시킨다.
        found->second.state->revision = ++mStateRevision;
        ++found->second.state->meaningfulRevision;
        found->second.statePending = true;
    }
    found->second.preparedState.reset();
    found->second.preparedEnter.reset();
    Broadcast("ProfileChanged", changed, SessionId::Invalid, true);
    return Status::Ok();
}

Status SummitServerBackend::HandlePlayerStateImpl(
    const std::shared_ptr<Session>& session,
    const ServerCore::Protocol::Message& message, const bool fromDatagram)
{
    if (!session)
    {
        return Status::Fail(ErrorCode::InvalidArgument, "PlayerState requires a session");
    }

    const SessionId id = session->Id();
    if (session->State() != SessionState::Authenticated || mPlayers.find(id) == mPlayers.end())
    {
        return SendJoinRejected(session, "protocol_violation", message.Sequence(), true);
    }

    Player& player = mPlayers.at(id);
    // 협상된 세션의 입력을 TCP/UDP가 경쟁하지 않게 한다. 협상하지 않은 기존 클라이언트는
    // 계속 TCP 경로만 사용하며, UDP 선택 세션은 첫 상태부터 UDP만 사용한다.
    if (!fromDatagram && player.udpMovement) return Status::Ok();
    const JsonValue& body = *message.Body();
    const JsonValue* const x = FindFiniteFloatNumber(body, "x");
    const JsonValue* const y = FindFiniteFloatNumber(body, "y");
    const JsonValue* const velocityX = FindFiniteFloatNumber(body, "vx");
    const JsonValue* const velocityY = FindFiniteFloatNumber(body, "vy");
    const JsonValue* const facing = FindFiniteFloatNumber(body, "f");
    const JsonValue* const stateValue = body.Find("s");
    const std::string* const state = stateValue ? stateValue->TryString() : nullptr;
    std::uint64_t character = 0;
    const bool characterValid = ReadUnsignedInteger(body.Find("c"), character);
    std::uint64_t sequence = 0;
    const JsonValue* const sequenceValue = body.Find("q");
    const bool sequenceValid = (!sequenceValue && !fromDatagram) || ReadMovementSequence(sequenceValue, sequence);

    if (x == nullptr || y == nullptr || velocityX == nullptr || velocityY == nullptr ||
        facing == nullptr || state == nullptr || state->size() > 32 || !characterValid ||
        character > NicknameCharacter || !sequenceValid ||
        (x && std::abs(*x->TryNumber()) > MaximumWorldCoordinate) ||
        (y && std::abs(*y->TryNumber()) > MaximumWorldCoordinate))
    {
        const Status invalid = InvalidFormat("PlayerState has invalid or missing fields");
        if (!fromDatagram) session->Disconnect(invalid);
        return invalid;
    }

    if (fromDatagram && player.state && sequence <= player.state->sequence) return Status::Ok();
    if (mStateRevision == (std::numeric_limits<std::uint64_t>::max)())
    {
        if (!fromDatagram) session->Disconnect(Status::FailWithoutMessage(ErrorCode::TooLarge));
        return Status::FailWithoutMessage(ErrorCode::TooLarge);
    }
    Player::State next{*x->TryNumber(), *y->TryNumber(), *velocityX->TryNumber(),
        *velocityY->TryNumber(), *facing->TryNumber(), *state,
        ServerCore::Core::MillisecondsSinceProcessStart(), sequence,
        mStateRevision + 1};
    // q/t만 갱신된 heartbeat는 의미 변화 보너스를 얻지 않는다. 위치·속도·방향·애니메이션의
    // 변화 세대만 edge에 남겨 마지막 전송 pose 전체를 가시 관계마다 복사하지 않는다.
    const bool hadPosition = player.state.has_value();
    const double previousX = hadPosition ? player.state->x : next.x;
    const double previousY = hadPosition ? player.state->y : next.y;
    const bool positionChanged = !hadPosition || next.x != previousX || next.y != previousY;
    const bool changed = !player.state || next.x != player.state->x || next.y != player.state->y ||
        next.vx != player.state->vx || next.vy != player.state->vy || next.facing != player.state->facing ||
        next.animation != player.state->animation;
    next.meaningfulRevision = player.state ? player.state->meaningfulRevision + (changed ? 1 : 0) : 1;
    const Cell nextCell = StateCell(next);
    // 새 셀 삽입까지 성공한 뒤 기존 셀을 뺀다. 할당 실패가 유효 상태와 공간 인덱스를
    // 서로 다른 위치에 남기지 않으며 floor 이전의 월드 범위 검사로 정수 변환을 제한한다.
    auto [newCell, insertedCell] = mCells.try_emplace(nextCell);
    try { newCell->second.insert(id); }
    catch (...)
    {
        if (insertedCell) mCells.erase(newCell);
        throw;
    }
    if (player.state && !(StateCell(*player.state) == nextCell))
    {
        const auto old = mCells.find(StateCell(*player.state));
        if (old != mCells.end())
        {
            old->second.erase(id);
            if (old->second.empty()) mCells.erase(old);
        }
    }
    if (player.statePending) ++mAoiMetrics.coalescedStates;
    player.state = std::move(next);
    if (positionChanged)
    {
        // 같은 셀 안의 이동도 사각형 경계를 넘을 수 있다. 이전 위치의 관찰자는 Exit,
        // 새 위치의 관찰자는 Enter를 재평가하고, 움직인 자신은 전체 가시 집합을 갱신한다.
        player.visibilityDirty = true;
        if (hadPosition) MarkVisibilityDirtyAround(previousX, previousY);
        MarkVisibilityDirtyAround(player.state->x, player.state->y);
    }
    ++mStateRevision;
    player.preparedState.reset();
    player.preparedEnter.reset();
    player.statePending = true;
    return Status::Ok();
}

Status SummitServerBackend::HandleChatImpl(
    const std::shared_ptr<Session>& session,
    const ServerCore::Protocol::Message& message)
{
    if (!session)
    {
        return Status::Fail(ErrorCode::InvalidArgument, "Chat requires a session");
    }
    const auto player = mPlayers.find(session->Id());
    if (session->State() != SessionState::Authenticated || player == mPlayers.end())
    {
        return SendJoinRejected(session, "protocol_violation", message.Sequence(), true);
    }

    const JsonValue& body = *message.Body();
    const JsonValue* const textValue = body.Find("text");
    const std::string* const text = textValue ? textValue->TryString() : nullptr;
    const std::uint64_t now = ServerCore::Core::MillisecondsSinceProcessStart();
    // 거절된 요청은 마지막 승인 시각을 바꾸지 않는다. 첫 채팅은 즉시 허용하며, 재시도가
    // 차단 시간을 계속 늘리거나 잘못된 텍스트가 다음 정상 채팅의 기회를 소모하지 않는다.
    const auto& lastChat = player->second.lastChatMilliseconds;
    const char* rejection = nullptr;
    if (!IsChatTextValid(text))
    {
        rejection = "chat_invalid";
    }
    else if (lastChat && now - *lastChat < ChatCooldownMilliseconds)
    {
        rejection = "chat_rate_limited";
    }
    if (rejection)
    {
        JsonValue::Object fields;
        fields.emplace("code", JsonValue(std::string(rejection)));
        const JsonValue error(std::move(fields));
        const Status sent = session->Send(ServerCore::Protocol::MessageFields{
            "ChatRejected", nullptr, message.Sequence(), &error });
        if (!sent.IsOk()) session->Disconnect(sent);
        return sent;
    }

    JsonValue::Object relayFields;
    relayFields.emplace("id", JsonValue(WireSessionId(session->Id())));
    relayFields.emplace("name", JsonValue(player->second.name));
    relayFields.emplace("text", JsonValue(*text));
    relayFields.emplace("t", JsonValue(now));
    const JsonValue relay(std::move(relayFields));
    // id/name/t는 요청 필드를 복사하지 않는다. 현재 가입 세션과 승인 프로필이 작성자를 정한다.
    player->second.lastChatMilliseconds = now;
    LogGameplayEvent("chat", [&]() { return *relay.TryObject(); });
    // Each event owns the approved name at this point. Later profile changes cannot rewrite it.
    // Include the sender as the acceptance echo; retain no history for future joins.
    Broadcast("ChatMessage", relay, SessionId::Invalid, true);
    return Status::Ok();
}

std::size_t SummitServerBackend::CellHash::operator()(const Cell& cell) const noexcept
{
    const auto x = static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell.x));
    const auto y = static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell.y));
    return std::hash<std::uint64_t>{}((x << 32) | y);
}

SummitServerBackend::Cell SummitServerBackend::StateCell(const Player::State& state) noexcept
{
    return Cell{static_cast<int>(std::floor(state.x / CellSize)),
        static_cast<int>(std::floor(state.y / CellSize))};
}

JsonValue SummitServerBackend::MakeProfile(const SessionId id, const Player& player)
{
    JsonValue::Object fields;
    fields.emplace("id", JsonValue(WireSessionId(id)));
    fields.emplace("name", JsonValue(player.name));
    fields.emplace("c", JsonValue(player.character));
    return JsonValue(std::move(fields));
}

JsonValue SummitServerBackend::MakeState(const SessionId id, const Player& player, const bool includeName)
{
    const Player::State& state = *player.state;
    JsonValue::Object fields;
    fields.emplace("id", JsonValue(WireSessionId(id)));
    if (includeName) fields.emplace("name", JsonValue(player.name));
    fields.emplace("c", JsonValue(player.character));
    fields.emplace("x", JsonValue(state.x));
    fields.emplace("y", JsonValue(state.y));
    fields.emplace("vx", JsonValue(state.vx));
    fields.emplace("vy", JsonValue(state.vy));
    fields.emplace("f", JsonValue(state.facing));
    fields.emplace("s", JsonValue(state.animation));
    fields.emplace("t", JsonValue(state.time));
    fields.emplace("q", JsonValue(state.sequence));
    fields.emplace("r", JsonValue(std::to_string(state.revision)));
    return JsonValue(std::move(fields));
}

Status SummitServerBackend::HandleHeartbeat(const std::shared_ptr<Session>& session,
    const ServerCore::Protocol::Message& message) noexcept
{
    try
    {
        const Operation operation(*this);
        if (!session) return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        if (session->State() != SessionState::Authenticated || !mPlayers.contains(session->Id()))
            return SendJoinRejected(session, "protocol_violation", message.Sequence(), true);
        if (!message.Body() || !message.Body()->TryObject())
            return Status::FailWithoutMessage(ErrorCode::InvalidFormat);
        // 이동은 UDP여도 TCP 제어 세션의 idle timeout은 유효하다. 응답 없이 정상 프레임
        // 수신 자체가 Host의 활동 시각을 갱신하며, 별도 게임 상태나 채팅을 만들지 않는다.
        return Status::Ok();
    }
    catch (...) { return Status::AllocationFailure(); }
}

Status SummitServerBackend::HandleDirectoryAck(const std::shared_ptr<Session>& session,
    const ServerCore::Protocol::Message& message) noexcept
{
    try
    {
        const Operation operation(*this);
        if (!session) return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        const auto found = mPlayers.find(session->Id());
        if (session->State() != SessionState::Authenticated || found == mPlayers.end())
            return SendJoinRejected(session, "protocol_violation", message.Sequence(), true);
        Player& player = found->second;
        const JsonValue* value = message.Body()->Find("cursor");
        const std::string* text = value ? value->TryString() : nullptr;
        std::uint64_t cursor = 0;
        bool valid = text && !text->empty() && text->size() <= 20;
        if (valid)
        {
            const auto parsed = std::from_chars(text->data(), text->data() + text->size(), cursor);
            valid = parsed.ec == std::errc{} && parsed.ptr == text->data() + text->size() &&
                (text->size() == 1 || text->front() != '0');
        }
        if (!valid || !player.directoryWaitingAck ||
            cursor != NumericSessionId(player.directoryCursor))
        {
            const Status invalid = InvalidFormat("DirectoryAck does not match the outstanding page");
            session->Disconnect(invalid);
            return invalid;
        }
        // 정확한 ACK만 창을 비운다. 중복/다른 cursor는 deadline을 갱신하지 않는다.
        player.directoryWaitingAck = false;
        return Status::Ok();
    }
    catch (...)
    {
        if (session) session->Disconnect(Status::AllocationFailure());
        return Status::AllocationFailure();
    }
}

ServerCore::Core::Result<const PreparedJsonValue*> SummitServerBackend::PrepareState(
    const SessionId id, Player& player, const bool includeName)
{
    using Result = ServerCore::Core::Result<const PreparedJsonValue*>;
    auto& cache = includeName ? player.preparedEnter : player.preparedState;
    if (!cache)
    {
        auto prepared = ServerCore::Protocol::PrepareJsonValue(MakeState(id, player, includeName));
        if (!prepared.IsOk()) return Result::FromStatus(std::move(prepared).TakeStatus());
        cache.emplace(std::move(prepared.Value()));
        ++mAoiMetrics.preparedStateItems;
    }
    return Result::FromValue(&*cache);
}

bool SummitServerBackend::SendControl(const std::shared_ptr<Session>& session,
    const PreparedMessage& message, std::size_t& clientBytes, std::size_t& globalBytes)
{
    const std::size_t bytes = message.Size() + 4;
    if (message.Size() > MaximumBodyBytes)
    {
        session->Disconnect(Status::FailWithoutMessage(ErrorCode::TooLarge));
        return false;
    }
    if (bytes > clientBytes || bytes > globalBytes)
    {
        mControlBudgetLimited = true;
        return false;
    }
    const Status sent = session->SendPrepared(message);
    if (!sent.IsOk())
    {
        if (sent.Code() == ErrorCode::WouldBlock) ++mAoiMetrics.controlBackpressureCount;
        else session->Disconnect(sent);
        return false;
    }
    clientBytes -= bytes;
    globalBytes -= bytes;
    mAoiMetrics.sentControlBytes += bytes;
    mAoiMetrics.lastTickControlBytes += bytes;
    return true;
}

void SummitServerBackend::AdvanceDirectory(const SessionId id, Player& player,
    const std::shared_ptr<Session>& session, const std::uint64_t now,
    std::size_t& clientBytes, std::size_t& globalBytes)
{
    if (player.directoryWaitingAck) return;
    JsonValue::Array profiles;
    std::vector<SessionId> ids;
    profiles.reserve(MaximumBatchItems);
    ids.reserve(MaximumBatchItems);
    for (auto next = mPlayers.upper_bound(player.directoryCursor);
        next != mPlayers.end() && next->first <= player.directoryThroughId &&
        profiles.size() < MaximumBatchItems; ++next)
    {
        if (next->first == id) continue;
        profiles.push_back(MakeProfile(next->first, next->second));
        ids.push_back(next->first);
    }
    if (profiles.empty())
    {
        const JsonValue body(JsonValue::Object{});
        auto prepared = ServerCore::Protocol::PrepareMessage({"DirectoryReady", &body, nullptr, nullptr});
        if (!prepared.IsOk()) session->Disconnect(std::move(prepared).TakeStatus());
        else if (SendControl(session, prepared.Value(), clientBytes, globalBytes)) player.directoryReady = true;
        return;
    }
    // 미송신 페이지는 보관하지 않는다. 다음 기회에 최신 승인 프로필을 다시 읽고, 성공한
    // cursor만 전진시킨다. ACK 창은 그대로 한 장이며 대기 예산이 별도 과거 명단 FIFO가 아니다.
    const std::size_t limit = (std::min)({static_cast<std::size_t>(MaximumBodyBytes) + 4, clientBytes, globalBytes});
    for (;;)
    {
        JsonValue::Object fields;
        fields.emplace("players", JsonValue(profiles));
        fields.emplace("cursor", JsonValue(WireSessionId(ids.back())));
        const JsonValue body(std::move(fields));
        auto prepared = ServerCore::Protocol::PrepareMessage({"DirectoryPage", &body, nullptr, nullptr});
        if (!prepared.IsOk())
        {
            session->Disconnect(std::move(prepared).TakeStatus());
            return;
        }
        if (prepared.Value().Size() + 4 > limit)
        {
            if (profiles.size() == 1)
            {
                if (prepared.Value().Size() > MaximumBodyBytes)
                    session->Disconnect(Status::FailWithoutMessage(ErrorCode::TooLarge));
                else mControlBudgetLimited = true;
                return;
            }
            profiles.resize(profiles.size() / 2);
            ids.resize(profiles.size());
            continue;
        }
        if (SendControl(session, prepared.Value(), clientBytes, globalBytes))
        {
            player.directoryCursor = ids.back();
            player.directoryWaitingAck = true;
            player.directorySentAtMilliseconds = now;
        }
        return;
    }
}

void SummitServerBackend::MarkVisibilityDirtyAround(const double x, const double y) noexcept
{
    // 관찰자의 Exit 사각형은 대상 중심으로 뒤집어도 같은 범위다. coarse cell을 모두
    // 표시하므로 경계/음수/순간이동을 놓치지 않으며 실제 포함 여부는 복제 시 다시 검사한다.
    const int minX = static_cast<int>(std::floor((x - ExitHalfWidth) / CellSize));
    const int maxX = static_cast<int>(std::floor((x + ExitHalfWidth) / CellSize));
    const int minY = static_cast<int>(std::floor((y - ExitHalfHeight) / CellSize));
    const int maxY = static_cast<int>(std::floor((y + ExitHalfHeight) / CellSize));
    for (int cellY = minY; cellY <= maxY; ++cellY)
        for (int cellX = minX; cellX <= maxX; ++cellX)
        {
            const auto cell = mCells.find(Cell{cellX, cellY});
            if (cell == mCells.end()) continue;
            for (const SessionId observer : cell->second)
            {
                const auto found = mPlayers.find(observer);
                if (found != mPlayers.end()) found->second.visibilityDirty = true;
            }
        }
}

void SummitServerBackend::ReplicateVisibility(const SessionId id, Player& player,
    const std::shared_ptr<Session>& session, std::size_t& clientBytes, std::size_t& globalBytes)
{
    if (player.visibilityReady && !player.visibilityDirty) return;
    const Player::State& origin = *player.state;
    std::unordered_set<SessionId, SessionIdHash> desired;
    desired.reserve(player.visible.size());
    const int minX = static_cast<int>(std::floor((origin.x - ExitHalfWidth) / CellSize));
    const int maxX = static_cast<int>(std::floor((origin.x + ExitHalfWidth) / CellSize));
    const int minY = static_cast<int>(std::floor((origin.y - ExitHalfHeight) / CellSize));
    const int maxY = static_cast<int>(std::floor((origin.y + ExitHalfHeight) / CellSize));
    for (int cellY = minY; cellY <= maxY; ++cellY)
        for (int cellX = minX; cellX <= maxX; ++cellX)
        {
            const auto cell = mCells.find(Cell{cellX, cellY});
            if (cell == mCells.end()) continue;
            for (const SessionId candidate : cell->second)
            {
                if (candidate == id) continue;
                const Player& entity = mPlayers.at(candidate);
                if (!entity.state || !IsWithinView(origin.x, origin.y, entity.state->x,
                        entity.state->y, player.visible.contains(candidate))) continue;
                if (desired.size() == MaximumVisiblePerPlayer)
                {
                    session->Disconnect(Status::FailWithoutMessage(ErrorCode::TooLarge));
                    return;
                }
                desired.insert(candidate);
            }
        }
    if (mVisibleEdges - player.visible.size() + desired.size() > MaximumVisibleEdges)
    {
        session->Disconnect(Status::FailWithoutMessage(ErrorCode::TooLarge));
        return;
    }

    std::vector<SessionId> exits, enters;
    for (const auto& [target, visible] : player.visible)
    {
        (void)visible;
        if (!desired.contains(target)) exits.push_back(target);
    }
    for (const SessionId target : desired)
        if (!player.visible.contains(target)) enters.push_back(target);
    std::sort(exits.begin(), exits.end());
    std::sort(enters.begin(), enters.end());

    // Exit를 먼저 완료한다. 제어 예산/큐가 막히면 다음 tick의 현재 desired를 재평가하며,
    // 실제로 수락된 batch만 visible에 반영한다. 아직 Enter하지 않은 대상은 상태 후보가 아니다.
    for (std::size_t offset = 0; offset < exits.size();)
    {
        auto empty = ServerCore::Protocol::PrepareArrayMessage("VisibilityExit", "ids", {});
        if (!empty.IsOk()) { session->Disconnect(std::move(empty).TakeStatus()); return; }
        const std::size_t limit = (std::min)({static_cast<std::size_t>(MaximumBodyBytes) + 4, clientBytes, globalBytes});
        std::size_t bytes = empty.Value().Size() + 4;
        std::vector<PreparedJsonValue> values;
        std::vector<const PreparedJsonValue*> items;
        values.reserve(MaximumBatchItems);
        items.reserve(MaximumBatchItems);
        while (offset + items.size() < exits.size() && items.size() < MaximumBatchItems)
        {
            auto value = ServerCore::Protocol::PrepareJsonValue(JsonValue(WireSessionId(exits[offset + items.size()])));
            if (!value.IsOk()) { session->Disconnect(std::move(value).TakeStatus()); return; }
            const std::size_t nextBytes = value.Value().Size() + (items.empty() ? 0 : 1);
            if (bytes > limit || nextBytes > limit - bytes) break;
            bytes += nextBytes;
            values.push_back(std::move(value.Value()));
            items.push_back(&values.back());
        }
        if (items.empty()) { mControlBudgetLimited = true; return; }
        auto prepared = ServerCore::Protocol::PrepareArrayMessage("VisibilityExit", "ids", items);
        if (!prepared.IsOk()) { session->Disconnect(std::move(prepared).TakeStatus()); return; }
        if (!SendControl(session, prepared.Value(), clientBytes, globalBytes)) return;
        for (std::size_t index = 0; index < items.size(); ++index)
            mVisibleEdges -= player.visible.erase(exits[offset + index]);
        offset += items.size();
    }

    for (std::size_t offset = 0; offset < enters.size();)
    {
        auto empty = ServerCore::Protocol::PrepareArrayMessage("VisibilityEnter", "players", {});
        if (!empty.IsOk()) { session->Disconnect(std::move(empty).TakeStatus()); return; }
        const std::size_t limit = (std::min)({static_cast<std::size_t>(MaximumBodyBytes) + 4, clientBytes, globalBytes});
        std::size_t bytes = empty.Value().Size() + 4;
        std::vector<const PreparedJsonValue*> items;
        items.reserve(MaximumBatchItems);
        while (offset + items.size() < enters.size() && items.size() < MaximumBatchItems)
        {
            const SessionId target = enters[offset + items.size()];
            auto value = PrepareState(target, mPlayers.at(target), true);
            if (!value.IsOk()) { session->Disconnect(std::move(value).TakeStatus()); return; }
            const std::size_t nextBytes = value.Value()->Size() + (items.empty() ? 0 : 1);
            if (bytes > limit || nextBytes > limit - bytes) break;
            bytes += nextBytes;
            items.push_back(value.Value());
        }
        if (items.empty()) { mControlBudgetLimited = true; return; }
        auto prepared = ServerCore::Protocol::PrepareArrayMessage("VisibilityEnter", "players", items);
        if (!prepared.IsOk()) { session->Disconnect(std::move(prepared).TakeStatus()); return; }
        // 노드를 먼저 할당한다. 송신 성공 뒤 메모리 부족으로 클라이언트와 명단이 갈라지는
        // 창을 만들지 않으며, 큐 거절/예외는 준비한 노드만 되돌린다.
        struct Additions
        {
            decltype(player.visible)& visible;
            const std::vector<SessionId>& ids;
            std::size_t offset;
            std::size_t inserted = 0;
            bool committed = false;
            ~Additions()
            {
                if (!committed)
                    for (std::size_t index = 0; index < inserted; ++index) visible.erase(ids[offset + index]);
            }
        } additions{player.visible, enters, offset};
        for (std::size_t index = 0; index < items.size(); ++index)
        {
            const SessionId target = enters[offset + index];
            const auto& state = *mPlayers.at(target).state;
            player.visible.emplace(target, Player::VisibleState{state.revision, state.meaningfulRevision, std::nullopt, mSchedulerNow});
            ++additions.inserted;
        }
        if (!SendControl(session, prepared.Value(), clientBytes, globalBytes)) return;
        mVisibleEdges += items.size();
        additions.committed = true;
        offset += items.size();
    }
    if (!player.visibilityReady)
    {
        const JsonValue body(JsonValue::Object{});
        auto prepared = ServerCore::Protocol::PrepareMessage({"VisibilityReady", &body, nullptr, nullptr});
        if (!prepared.IsOk()) session->Disconnect(std::move(prepared).TakeStatus());
        else if (SendControl(session, prepared.Value(), clientBytes, globalBytes)) player.visibilityReady = true;
    }
    // 중간 return/큐 거절은 dirty를 남긴다. 미완료 Enter/Exit/Ready는 좌표가 그대로여도 재시도한다.
    if (player.visibilityReady) player.visibilityDirty = false;
}

void SummitServerBackend::ReplicateState(Player& player, const std::shared_ptr<Session>& session,
    std::size_t& globalBytes, const std::uint64_t now)
{
    const bool datagrams = player.udpMovement && mMovementTransport;
    if (datagrams && !mMovementTransport->IsReady(session->Id())) return;
    const std::size_t overhead = datagrams ? ServerCore::Protocol::DatagramCodec::HeaderBytes : 4;
    const std::size_t maximumBytes = datagrams ? ServerCore::Protocol::DatagramCodec::MaximumDatagramBytes :
        static_cast<std::size_t>(MaximumBodyBytes) + overhead;
    struct Candidate
    {
        SessionId id;
        double priority;
        std::uint64_t dirtySince;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(player.visible.size());
    for (auto& [target, visible] : player.visible)
    {
        const auto found = mPlayers.find(target);
        if (found == mPlayers.end() || !found->second.state) continue;
        const auto& state = *found->second.state;
        // Exit 전송이 예산 때문에 보류 중이어도 범위 밖의 새 이동을 보내지는 않는다.
        const bool refreshDue = datagrams && now >= visible.lastSentAtMilliseconds &&
            now - visible.lastSentAtMilliseconds >= MovementResendMilliseconds;
        if (!IsWithinView(player.state->x, player.state->y, state.x, state.y, true) ||
            (visible.revision == state.revision && !refreshDue)) continue;
        if (!visible.dirtySince) visible.dirtySince = now;
        const std::uint64_t age = now >= *visible.dirtySince ? now - *visible.dirtySince : 0;
        const double distance = (std::max)(std::abs(state.x - player.state->x) / ExitHalfWidth,
            std::abs(state.y - player.state->y) / ExitHalfHeight);
        const double nearBonus = 250.0 * (std::max)(0.0, 1.0 - distance);
        const double changeBonus = visible.meaningfulRevision != state.meaningfulRevision ? 500.0 : 0.0;
        candidates.push_back({target, static_cast<double>(age) + nearBonus + changeBonus, *visible.dirtySince});
    }
    // 보너스 합은 750ms를 넘지 않는다. 계속 움직이는 가까운 대상도 오래 밀린 정지 상태를
    // 영구히 앞서지 못하며, 동점은 최초 dirty 시각과 ID로 고정한다.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& first, const Candidate& second) {
        if (first.priority != second.priority) return first.priority > second.priority;
        if (first.dirtySince != second.dirtySince) return first.dirtySince < second.dirtySince;
        return first.id < second.id;
    });
    if (candidates.empty()) return;
    std::size_t clientBytes = mSchedulerOptions.perClientStateBytesPerTick;
    const std::size_t backlogLimit = (std::max)(std::size_t{8192}, clientBytes * 4);
    auto empty = ServerCore::Protocol::PrepareArrayMessage("StateBatch", "states", {});
    if (!empty.IsOk()) { session->Disconnect(std::move(empty).TakeStatus()); return; }
    for (std::size_t offset = 0; offset < candidates.size();)
    {
        const std::size_t limit = (std::min)({maximumBytes, clientBytes, globalBytes});
        std::size_t bytes = empty.Value().Size() + overhead;
        std::vector<const PreparedJsonValue*> items;
        items.reserve(MaximumBatchItems);
        while (offset + items.size() < candidates.size() && items.size() < MaximumBatchItems)
        {
            const SessionId target = candidates[offset + items.size()].id;
            auto value = PrepareState(target, mPlayers.at(target), false);
            if (!value.IsOk()) { session->Disconnect(std::move(value).TakeStatus()); return; }
            const std::size_t nextBytes = value.Value()->Size() + (items.empty() ? 0 : 1);
            if (bytes > limit || nextBytes > limit - bytes) break;
            bytes += nextBytes;
            items.push_back(value.Value());
        }
        if (items.empty()) { mStateBudgetLimited = true; return; }
        // 큐는 관측 뒤 변할 수 있다. 이 검사는 새 오래된 패킷의 누적을 줄이는 admission
        // 정책이고, 실제 수락과 한도 집행은 Session/Connection의 원자적 송신 경계가 담당한다.
        const std::size_t queued = datagrams ? 0 : session->QueuedSendBytes();
        if (!datagrams && (queued > backlogLimit || bytes > backlogLimit - queued))
        {
            ++mAoiMetrics.stateBackpressureCount;
            return;
        }
        auto prepared = ServerCore::Protocol::PrepareArrayMessage("StateBatch", "states", items);
        if (!prepared.IsOk()) { session->Disconnect(std::move(prepared).TakeStatus()); return; }
        SERVERCORE_ASSERT(prepared.Value().Size() + overhead == bytes, "prepared array byte accounting drifted");
        const Status sent = datagrams ? mMovementTransport->SendState(session->Id(), prepared.Value()) :
            session->SendPrepared(prepared.Value());
        if (!sent.IsOk())
        {
            // UDP 실패는 TCP 신원/제어를 종료하지 않는다. 성공한 datagram도 도착 보장이
            // 아니므로 500ms 뒤 최신 상태를 다시 후보로 삼아 마지막 정지 패킷 유실을 치유한다.
            if (datagrams || sent.Code() == ErrorCode::WouldBlock) ++mAoiMetrics.stateBackpressureCount;
            else session->Disconnect(sent);
            return;
        }
        clientBytes -= bytes;
        globalBytes -= bytes;
        ++mAoiMetrics.sentStateFrames;
        mAoiMetrics.sentStateItems += items.size();
        mAoiMetrics.sentStateBytes += bytes;
        mAoiMetrics.lastTickStateBytes += bytes;
        for (std::size_t index = 0; index < items.size(); ++index)
        {
            const SessionId target = candidates[offset + index].id;
            auto& visible = player.visible.at(target);
            const auto& state = *mPlayers.at(target).state;
            visible.revision = state.revision;
            visible.meaningfulRevision = state.meaningfulRevision;
            visible.dirtySince.reset();
            visible.lastSentAtMilliseconds = now;
        }
        offset += items.size();
    }
}

void SummitServerBackend::Tick(const std::uint64_t nowMilliseconds) noexcept
{
    try
    {
        const Operation operation(*this);
        struct MeasureTick
        {
            AoiMetrics& metrics;
            std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
            ~MeasureTick()
            {
                // 관측 전용이다. Operation 획득 대기/마지막 deferred-close drain은 제외한다.
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count();
                metrics.lastTickDurationMicroseconds = static_cast<std::uint64_t>((std::max)(elapsed, decltype(elapsed){0}));
                metrics.maxTickDurationMicroseconds = (std::max)(metrics.maxTickDurationMicroseconds, metrics.lastTickDurationMicroseconds);
            }
        } measurement{mAoiMetrics};
        ++mAoiMetrics.tickCount;
        mAoiMetrics.lastTickStateBytes = 0;
        mAoiMetrics.lastTickControlBytes = 0;
        mStateBudgetLimited = false;
        mControlBudgetLimited = false;
        mSchedulerNow = (std::max)(mSchedulerNow, nowMilliseconds);
        // 스케줄링 예산이 부족해도 모든 가입/ACK 기한은 매 tick 검사한다.
        for (auto& [id, player] : mPlayers)
        {
            (void)id;
            const auto session = player.session.lock();
            if (!session || session->State() != SessionState::Authenticated) continue;
            const bool noPosition = !player.state && mSchedulerNow >= player.joinedAtMilliseconds &&
                mSchedulerNow - player.joinedAtMilliseconds >= SynchronizationTimeoutMilliseconds;
            const bool noAck = player.directoryWaitingAck && mSchedulerNow >= player.directorySentAtMilliseconds &&
                mSchedulerNow - player.directorySentAtMilliseconds >= SynchronizationTimeoutMilliseconds;
            if (noPosition || noAck) session->Disconnect(Status::FailWithoutMessage(ErrorCode::Timeout));
        }

        const auto visit = [&](SessionId& nextRecipient, std::size_t& bytes, bool& limited, auto&& callback) {
            if (mPlayers.empty()) { nextRecipient = SessionId::Invalid; return; }
            auto next = mPlayers.lower_bound(nextRecipient);
            if (next == mPlayers.end()) next = mPlayers.begin();
            const std::size_t count = mPlayers.size();
            for (std::size_t visited = 0; visited < count; ++visited)
            {
                // 남은 조각을 쓰려고 전체 가시 그래프를 다시 훑지 않는다. 다음 기회에는
                // 멈춘 바로 다음 수신자가 완전한 예산을 먼저 받아 ID 뒤쪽도 계속 전진한다.
                if (bytes < MinimumSchedulerBudget) { limited = true; break; }
                auto current = next++;
                if (next == mPlayers.end()) next = mPlayers.begin();
                nextRecipient = next->first;
                auto& [id, player] = *current;
                const auto session = player.session.lock();
                if (!session || session->State() != SessionState::Authenticated) continue;
                try { callback(id, player, session); }
                catch (...) { session->Disconnect(Status::AllocationFailure()); }
            }
        };
        // 제어와 이동은 독립된 RR 커서와 예산이다. 이동이 큰 수신자가 명단 동기화를 막지 않는다.
        std::size_t controlBytes = mSchedulerOptions.globalControlBytesPerTick;
        visit(mNextControlRecipient, controlBytes, mControlBudgetLimited,
            [&](const SessionId id, Player& player, const std::shared_ptr<Session>& session) {
                std::size_t clientBytes = mSchedulerOptions.perClientControlBytesPerTick;
                if (!player.directoryReady) AdvanceDirectory(id, player, session, mSchedulerNow, clientBytes, controlBytes);
                if (session->State() == SessionState::Authenticated && player.directoryReady && player.state)
                    ReplicateVisibility(id, player, session, clientBytes, controlBytes);
            });
        std::size_t stateBytes = mSchedulerOptions.globalStateBytesPerTick;
        visit(mNextStateRecipient, stateBytes, mStateBudgetLimited,
            [&](const SessionId, Player& player, const std::shared_ptr<Session>& session) {
                if (player.directoryReady && player.visibilityReady && player.state)
                    ReplicateState(player, session, stateBytes, mSchedulerNow);
            });
        if (mStateBudgetLimited) ++mAoiMetrics.stateBudgetLimitedTicks;
        if (mControlBudgetLimited) ++mAoiMetrics.controlBudgetLimitedTicks;
        for (auto& [id, player] : mPlayers)
        {
            (void)id;
            player.statePending = false;
        }
    }
    catch (...)
    {
        // Operation 획득 실패는 이 tick만 건너뛴다. 수신자별 준비/송신 실패는 위 경계에서
        // 정리하며, 이미 성공한 제어 batch와 상태 revision을 되돌리거나 재생하지 않는다.
    }
}

SummitServerBackend::AoiMetrics SummitServerBackend::SnapshotAoiMetrics() noexcept
{
    try
    {
        const Operation operation(*this);
        AoiMetrics result = mAoiMetrics;
        result.joinedPlayers = static_cast<std::uint64_t>(mPlayers.size());
        result.visibleEdges = static_cast<std::uint64_t>(mVisibleEdges);
        for (const auto& [id, player] : mPlayers)
        {
            (void)id;
            if (player.state) ++result.positionedPlayers;
        }
        return result;
    }
    catch (...) { return {}; }
}

ServerCore::Core::Result<std::vector<SummitServerBackend::PlayerInfo>>
SummitServerBackend::SnapshotPlayers() noexcept
{
    using Snapshot = ServerCore::Core::Result<std::vector<PlayerInfo>>;
    try
    {
        const Operation operation(*this);
        std::vector<PlayerInfo> players;
        players.reserve(mPlayers.size());
        for (const auto& [id, player] : mPlayers)
            players.push_back(PlayerInfo{ id, player.name, player.character });
        return Snapshot::FromValue(std::move(players));
    }
    catch (const std::bad_alloc&)
    {
        return Snapshot::FromStatus(Status::AllocationFailure());
    }
    catch (...)
    {
        return Snapshot::FromStatus(Status::FailWithoutMessage(ErrorCode::PlatformError));
    }
}

Status SummitServerBackend::Announce(const std::string_view text) noexcept
{
    try
    {
        const Operation operation(*this);
        if (text.size() > MaximumChatBytes)
        {
            return Status::Fail(ErrorCode::InvalidArgument, "announcement exceeds 512 UTF-8 bytes");
        }
        const std::string ownedText(text);
        if (!IsChatTextValid(&ownedText))
        {
            return Status::Fail(ErrorCode::InvalidArgument, "announcement is empty, blank, or contains controls");
        }
        // 운영자 입력은 ParseMessage를 거치지 않는다. 송신 전에 public JSON 검증 경로로
        // UTF-8도 확인해 잘못된 입력 때문에 정상 수신자를 끊는 일이 없도록 한다.
        auto validated = JsonValue(ownedText).Dump();
        if (!validated.IsOk()) return std::move(validated).TakeStatus();
        return PublishNotice("announcement", ownedText);
    }
    catch (const std::bad_alloc&)
    {
        return Status::AllocationFailure();
    }
    catch (...)
    {
        return Status::FailWithoutMessage(ErrorCode::PlatformError);
    }
}

Status SummitServerBackend::PublishNotice(const std::string_view kind,
    const std::string_view text, const SessionId id, const std::string_view name) noexcept
{
    // 호출자는 이미 Operation을 소유한다. 여기서 다시 획득하면 동기 재진입을 기다리며
    // 멈추므로, 준비/송신만 수행하고 종료 콜백은 기존 작업의 마지막에 처리한다.
    try
    {
        JsonValue::Object fields;
        fields.emplace("kind", JsonValue(std::string(kind)));
        fields.emplace("text", JsonValue(std::string(text)));
        fields.emplace("t", JsonValue(ServerCore::Core::MillisecondsSinceProcessStart()));
        if (id != SessionId::Invalid)
        {
            fields.emplace("id", JsonValue(WireSessionId(id)));
            fields.emplace("name", JsonValue(std::string(name)));
        }
        const JsonValue body(std::move(fields));
        Broadcast("ServerNotice", body, SessionId::Invalid, true);
        if (kind == "announcement")
        {
            LogGameplayEvent("announcement", [&]() { return *body.TryObject(); });
        }
        return Status::Ok();
    }
    catch (const std::bad_alloc&)
    {
        return Status::AllocationFailure();
    }
    catch (...)
    {
        return Status::FailWithoutMessage(ErrorCode::PlatformError);
    }
}

void SummitServerBackend::PublishMembershipNotice(const bool joined,
    const SessionId id, const std::string_view name) noexcept
{
    try
    {
        std::string text(name);
        text += joined ? "님이 입장했습니다." : "님이 퇴장했습니다.";
        // 채팅 알림을 만들지 못해도 이미 확정된 명단을 롤백하거나 전체 연결을 끊지 않는다.
        // 실제 membership 메시지는 별도 경로에서 먼저 처리되며, 알림 이력은 보관하지 않는다.
        const Status sent = PublishNotice(joined ? "join" : "leave", text, id, name);
        if (sent.IsOk()) return;
    }
    catch (...)
    {
    }
    ServerCore::Core::GetGlobalLogger().Write(ServerCore::Core::LogLevel::Warn,
        "could not construct Summit membership chat notice");
}

Status SummitServerBackend::SendJoinRejected(
    const std::shared_ptr<Session>& session,
    std::string code,
    const JsonValue* const sequence,
    const bool disconnect)
{
    JsonValue::Object errorFields;
    errorFields.emplace("code", JsonValue(std::move(code)));
    const JsonValue error(std::move(errorFields));
    const ServerCore::Protocol::MessageFields fields{
        "JoinRejected", nullptr, sequence, &error
    };

    // 수정 가능한 입력 오류는 같은 소켓에서 재시도한다. 이 응답은 PendingJoin의 최초 접속
    // 시각을 갱신하지 않으므로, 반복 거절만으로 가입 대기 자리를 무기한 차지할 수 없다.
    if (!disconnect)
    {
        return session->Send(fields);
    }

    return session->SendAndDisconnect(fields,
        InvalidFormat("the client violated the Summit join protocol"));
}

void SummitServerBackend::Broadcast(
    const std::string_view type,
    const JsonValue& body,
    const SessionId excludedId,
    const bool disconnectOnFailure)
{
    // 순회 전에 수신자 수명을 고정한다. Send가 동기적으로 종료를 알리더라도 현재 broadcast의
    // 나머지 수신자를 건너뛰지 않으며, 명단 변경은 Operation 종료 때 이어진다.
    SessionSnapshot recipients{ mRecipients };
    for (const auto& [id, player] : mPlayers)
    {
        if (id == excludedId)
        {
            continue;
        }
        if (const std::shared_ptr<Session> recipient = player.session.lock())
        {
            if (recipients.count < recipients.sessions.size())
            {
                recipients.sessions[recipients.count++] = recipient;
            }
        }
    }

    // 동일 프로필/채팅/공지 봉투의 JSON 구성·escape는 수신자 수와 무관하게 한 번만 한다.
    // 준비 실패 시에는 개별 Send 경로로 수신자별 실패와 예외를 처리한다.
    if (recipients.count == 0) return;
    auto prepared = ServerCore::Protocol::PrepareMessage({type, &body, nullptr, nullptr});
    for (std::size_t index = 0; index < recipients.count; ++index)
    {
        const std::shared_ptr<Session>& recipient = recipients.sessions[index];
        try
        {
            Status sent = prepared.IsOk() ? recipient->SendPrepared(prepared.Value())
                : recipient->Send(type, body);
            if (!sent.IsOk() && disconnectOnFailure)
            {
                // Membership, profile and chat events have no later resynchronization path.
                recipient->Disconnect(std::move(sent));
            }
        }
        catch (...)
        {
            // One receiver's allocation failure must not skip other receivers.
            recipient->Disconnect(Status::Fail(ErrorCode::PlatformError, std::string{}));
        }
    }
}
}
