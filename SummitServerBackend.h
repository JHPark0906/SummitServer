#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Dispatch/Dispatcher.h"
#include "ServerCore/Protocol/Json.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Session/Session.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Summit
{
class IMovementTransport;

/// Summit's deliberately small game-specific server layer.
///
/// Handlers, opens, and timeouts enter from ServerHost's JobRunner. A close can
/// also arrive from its I/O/finalizer fallback, including during Send/Disconnect.
/// The backend serializes complete operations and defers such closes until the
/// active operation finishes, preserving both map access and membership order.
/// The server owns joined identities and caches the latest client state for AOI replication;
/// it does not simulate movement or treat reaching the goal as a
/// session transition.
class SummitServerBackend final : public ServerCore::Session::ISessionObserver,
                                  public std::enable_shared_from_this<SummitServerBackend>
{
public:
    static constexpr std::size_t PlayerCapacity = 16;
    // 실행 인수를 지정하지 않았을 때의 작은 게임 기본값이다.
    static constexpr std::size_t ConnectionCapacity = 64;
    static constexpr std::size_t MaximumSessionCapacity = 65'536;
    static constexpr std::uint32_t MaximumBodyBytes = 8u * 1024u;
    static constexpr std::size_t MaximumNameBytes = 48;
    static constexpr std::uint64_t SchemaVersion = 6;
    static constexpr std::uint64_t JoinTimeoutMilliseconds = 30'000;
    static constexpr std::size_t MaximumChatBytes = 512;
    static constexpr std::uint64_t ChatCooldownMilliseconds = 500;
    static constexpr double CellSize = 16.0;
    static constexpr double EnterHalfWidth = 32.0;
    static constexpr double EnterHalfHeight = 20.0;
    static constexpr double ExitHalfWidth = 36.0;
    static constexpr double ExitHalfHeight = 24.0;
    static constexpr double MaximumWorldCoordinate = 1'000'000.0;
    static constexpr std::size_t MaximumBatchItems = 32;
    static constexpr std::size_t MaximumVisiblePerPlayer = 4096;
    static constexpr std::size_t MaximumVisibleEdges = 4'000'000;
    static constexpr std::uint64_t ReplicationIntervalMilliseconds = 50;
    static constexpr std::uint64_t SynchronizationTimeoutMilliseconds = 30'000;
    static constexpr std::uint64_t MovementResendMilliseconds = 500;

    struct SchedulerOptions
    {
        // TCP 길이 접두사 4바이트 또는 UDP 응용 헤더 28바이트까지 포함한 수락 바이트다. 제어 예산은 Directory/AOI만
        // 대상으로 하며 즉시 신뢰 전달하는 프로필·채팅·입퇴장 방송은 기존 큐 한도를 따른다.
        std::size_t perClientStateBytesPerTick = 2048;
        std::size_t globalStateBytesPerTick = 1024 * 1024;
        std::size_t perClientControlBytesPerTick = 16 * 1024;
        std::size_t globalControlBytesPerTick = 256 * 1024;
    };
    static constexpr std::size_t MinimumSchedulerBudget = 1024;
    static constexpr std::size_t MaximumSchedulerBudget = 16 * 1024 * 1024;

    // connectionCapacity는 Host의 maxConcurrentSessions와 일치해야 한다. 모든 종료 ID와
    // 외부 콜백용 스냅샷 저장소는 시작할 때 준비해 종료/OOM 경로에서는 할당하지 않는다.
    explicit SummitServerBackend(std::size_t playerCapacity = PlayerCapacity,
        std::size_t connectionCapacity = ConnectionCapacity);
    SummitServerBackend(std::size_t playerCapacity, std::size_t connectionCapacity,
        SchedulerOptions schedulerOptions);
    [[nodiscard]] std::size_t GetPlayerCapacity() const noexcept { return mPlayerCapacity; }
    [[nodiscard]] std::size_t GetConnectionCapacity() const noexcept { return mConnectionCapacity; }
    [[nodiscard]] const SchedulerOptions& GetSchedulerOptions() const noexcept { return mSchedulerOptions; }

    // 첫 OnSessionOpened 이전에 설정한다. TCP가 신원과 신뢰 제어의 수명을 계속 소유하며,
    // Join에서 명시적으로 선택한 세션만 이 전송을 등록한다.
    void SetMovementTransport(std::shared_ptr<IMovementTransport> transport);
    // transport가 토큰/패킷 순서를 검증한 뒤 잠금을 놓고 호출한다. UDP의 잘못되거나 오래된
    // 입력은 버리며 TCP 세션을 종료하지 않는다. 이 함수 자체가 Operation을 획득한다.
    void ReceiveMovement(ServerCore::Session::SessionId id,
        const ServerCore::Protocol::Message& message) noexcept;

    // shared_ptr로 소유한 뒤, Dispatcher가 Freeze되기 전에 한 번 등록한다.
    // 등록된 콜백은 weak_ptr만 보관하므로 백엔드의 종료 수명을 연장하지 않는다.
    [[nodiscard]] ServerCore::Core::Status RegisterHandlers(
        ServerCore::Dispatch::Dispatcher& dispatcher);

    void OnSessionOpened(
        const std::shared_ptr<ServerCore::Session::Session>& session) override;
    void OnSessionClosed(ServerCore::Session::SessionId id,
        ServerCore::Core::Status reason) noexcept override;

    /// Closes TCP sessions that never complete Join. The caller schedules this
    /// in the same ServerHost JobRunner context as handlers and open observers.
    void ExpireUnjoinedSessions(std::uint64_t nowMilliseconds) noexcept;

    // 운영자 입력을 소유한 작업을 Host JobRunner에 넣어 호출한다. 클라이언트용 handler는
    // 등록하지 않으며, 현재 가입자에게만 보낸다. 성공은 수신자의 실제 도착 확인이 아니다.
    // text는 유효한 UTF-8 1..512바이트이고 ASCII 공백만/C0/DEL은 허용하지 않는다.
    [[nodiscard]] ServerCore::Core::Status Announce(std::string_view text) noexcept;

    struct PlayerInfo
    {
        ServerCore::Session::SessionId id = ServerCore::Session::SessionId::Invalid;
        std::string name;
        std::uint64_t character = 0;
    };
    // Host JobRunner에서 독립 작업으로 호출한다(Session 콜백 안에서 재진입하지 않는다).
    // 승인된 명단을 ID 오름차순으로 복사하며, 반환 값은 세션이나 내부 문자열을 빌리지 않는다.
    // 조회 시점 이후의 변경은 반영하지 않는다. 시간/임시 메모리는 현재 가입 인원에 비례한다.
    [[nodiscard]] ServerCore::Core::Result<std::vector<PlayerInfo>> SnapshotPlayers() noexcept;

    // 50ms 주기로 Host JobRunner에서 호출한다. 최신 상태만 복제하며 TCP에 이미 들어간
    // 프레임은 고치지 않는다. 한도를 넘는 관찰자는 일부만 숨기는 대신 명시적으로 끊는다.
    void Tick(std::uint64_t nowMilliseconds) noexcept;
    struct AoiMetrics
    {
        std::uint64_t joinedPlayers = 0;
        std::uint64_t positionedPlayers = 0;
        std::uint64_t visibleEdges = 0;
        std::uint64_t coalescedStates = 0;
        std::uint64_t sentStateItems = 0;
        std::uint64_t sentStateFrames = 0;
        std::uint64_t tickCount = 0;
        std::uint64_t sentStateBytes = 0;
        std::uint64_t sentControlBytes = 0;
        std::uint64_t lastTickStateBytes = 0;
        std::uint64_t lastTickControlBytes = 0;
        std::uint64_t stateBudgetLimitedTicks = 0;
        std::uint64_t controlBudgetLimitedTicks = 0;
        std::uint64_t stateBackpressureCount = 0;
        std::uint64_t controlBackpressureCount = 0;
        std::uint64_t preparedStateItems = 0;
        std::uint64_t lastTickDurationMicroseconds = 0;
        std::uint64_t maxTickDurationMicroseconds = 0;
    };
    [[nodiscard]] AoiMetrics SnapshotAoiMetrics() noexcept;

private:
    class Operation final
    {
    public:
        explicit Operation(SummitServerBackend& backend);
        ~Operation();
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;

    private:
        SummitServerBackend& mBackend;
    };

    void BeginOperation();
    void FinishOperation() noexcept;
    void HandleSessionClosed(ServerCore::Session::SessionId id,
        ServerCore::Core::ErrorCode reason) noexcept;

    struct SessionIdHash
    {
        [[nodiscard]] std::size_t operator()(
            ServerCore::Session::SessionId id) const noexcept;
    };

    // 실제 연결 수명은 Host가 소유한다. 명단은 승인된 프로필과 약한 연결 참조만 보관한다.
    struct Player
    {
        std::string name;
        std::weak_ptr<ServerCore::Session::Session> session;
        std::uint64_t character = 0;
        std::optional<std::uint64_t> lastChatMilliseconds;
        std::uint64_t joinedAtMilliseconds = 0;
        ServerCore::Session::SessionId directoryThroughId = ServerCore::Session::SessionId::Invalid;
        ServerCore::Session::SessionId directoryCursor = ServerCore::Session::SessionId::Invalid;
        std::uint64_t directorySentAtMilliseconds = 0;
        bool directoryWaitingAck = false;
        bool directoryReady = false;
        bool visibilityReady = false;
        // 좌표/근처 구성원이 바뀌었거나 제어 batch가 미완료일 때만 가시 집합을 다시 계산한다.
        bool visibilityDirty = true;
        bool udpMovement = false;
        struct State
        {
            double x = 0, y = 0, vx = 0, vy = 0, facing = 1;
            std::string animation;
            std::uint64_t time = 0, sequence = 0, revision = 0;
            std::uint64_t meaningfulRevision = 0;
        };
        std::optional<State> state;
        bool statePending = false;
        struct VisibleState
        {
            std::uint64_t revision = 0;
            std::uint64_t meaningfulRevision = 0;
            // 최초로 미송신 revision을 관측한 때부터 기다린다. 긴 정지 뒤 새 입력이 오래된
            // dirty보다 앞서지 않으며, 성공한 큐 수락만 이 대기를 해제한다.
            std::optional<std::uint64_t> dirtySince;
            std::uint64_t lastSentAtMilliseconds = 0;
        };
        std::unordered_map<ServerCore::Session::SessionId, VisibleState, SessionIdHash> visible;
        // 최신 revision 하나만 준비한다. 동일 엔티티를 여러 수신자에게 보낼 때 JSON 객체
        // 구성/escape/숫자 인코딩을 반복하지 않으며 이전 상태의 패킷 FIFO는 보관하지 않는다.
        std::optional<ServerCore::Protocol::PreparedJsonValue> preparedState;
        std::optional<ServerCore::Protocol::PreparedJsonValue> preparedEnter;
    };

    struct Cell
    {
        int x = 0, y = 0;
        bool operator==(const Cell&) const noexcept = default;
    };
    struct CellHash
    {
        [[nodiscard]] std::size_t operator()(const Cell& cell) const noexcept;
    };

    struct PendingJoin
    {
        std::uint64_t openedAtMilliseconds = 0;
        std::weak_ptr<ServerCore::Session::Session> session;
    };

    [[nodiscard]] ServerCore::Core::Status HandleJoin(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message) noexcept;
    [[nodiscard]] ServerCore::Core::Status HandlePlayerState(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message) noexcept;
    [[nodiscard]] ServerCore::Core::Status HandleSetProfile(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message) noexcept;
    [[nodiscard]] ServerCore::Core::Status HandleChat(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message) noexcept;
    [[nodiscard]] ServerCore::Core::Status HandleDirectoryAck(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message) noexcept;
    [[nodiscard]] ServerCore::Core::Status HandleHeartbeat(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message) noexcept;
    void AdvanceDirectory(ServerCore::Session::SessionId id, Player& player,
        const std::shared_ptr<ServerCore::Session::Session>& session, std::uint64_t now,
        std::size_t& clientBytes, std::size_t& globalBytes);
    void MarkVisibilityDirtyAround(double x, double y) noexcept;
    void ReplicateVisibility(ServerCore::Session::SessionId id, Player& player,
        const std::shared_ptr<ServerCore::Session::Session>& session,
        std::size_t& clientBytes, std::size_t& globalBytes);
    void ReplicateState(Player& player, const std::shared_ptr<ServerCore::Session::Session>& session,
        std::size_t& globalBytes, std::uint64_t now);
    [[nodiscard]] ServerCore::Core::Result<const ServerCore::Protocol::PreparedJsonValue*> PrepareState(
        ServerCore::Session::SessionId id, Player& player, bool includeName);
    [[nodiscard]] bool SendControl(const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::PreparedMessage& message,
        std::size_t& clientBytes, std::size_t& globalBytes);
    [[nodiscard]] static Cell StateCell(const Player::State& state) noexcept;
    [[nodiscard]] static ServerCore::Protocol::JsonValue MakeProfile(
        ServerCore::Session::SessionId id, const Player& player);
    [[nodiscard]] static ServerCore::Protocol::JsonValue MakeState(
        ServerCore::Session::SessionId id, const Player& player, bool includeName);
    [[nodiscard]] ServerCore::Core::Status HandleChatImpl(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message);
    [[nodiscard]] ServerCore::Core::Status HandleSetProfileImpl(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message);

    [[nodiscard]] ServerCore::Core::Status HandleJoinImpl(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message);
    [[nodiscard]] ServerCore::Core::Status HandlePlayerStateImpl(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message, bool fromDatagram = false);

    [[nodiscard]] ServerCore::Core::Status SendJoinRejected(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        std::string code,
        const ServerCore::Protocol::JsonValue* sequence,
        bool disconnect);
    [[nodiscard]] ServerCore::Core::Status PublishNotice(std::string_view kind,
        std::string_view text,
        ServerCore::Session::SessionId id = ServerCore::Session::SessionId::Invalid,
        std::string_view name = {}) noexcept;
    void PublishMembershipNotice(bool joined, ServerCore::Session::SessionId id,
        std::string_view name) noexcept;
    void Broadcast(std::string_view type,
        const ServerCore::Protocol::JsonValue& body,
        ServerCore::Session::SessionId excludedId =
            ServerCore::Session::SessionId::Invalid,
        bool disconnectOnFailure = false);

    const std::size_t mPlayerCapacity;
    const std::size_t mConnectionCapacity;
    const SchedulerOptions mSchedulerOptions;
    std::shared_ptr<IMovementTransport> mMovementTransport;

    // Only the execution gate is protected by this mutex. Session calls always
    // run without it; a synchronous or concurrent close only queues its ID and
    // returns, so it never waits on the operation that caused the close.
    std::mutex mOperationMutex;
    std::condition_variable mOperationFinished;
    bool mOperationActive = false;
    static constexpr std::size_t NoDeferredClose = static_cast<std::size_t>(-1);
    struct DeferredClose
    {
        ServerCore::Session::SessionId id = ServerCore::Session::SessionId::Invalid;
        std::size_t nextInBucket = NoDeferredClose;
        ServerCore::Core::ErrorCode reason = ServerCore::Core::ErrorCode::Ok;
    };
    std::vector<DeferredClose> mDeferredCloses;
    // 큐 슬롯을 hash chain 노드로 재사용한다. 동시 대량 종료의 중복 검사가 전체 큐를 매번
    // 훑지 않으며, 삽입/제거 모두 미리 할당한 인덱스만 바꾼다.
    std::vector<std::size_t> mDeferredCloseBuckets;
    std::size_t mDeferredCloseBegin = 0;
    std::size_t mDeferredCloseCount = 0;

    std::vector<std::shared_ptr<ServerCore::Session::Session>> mRecipients;
    std::vector<std::shared_ptr<ServerCore::Session::Session>> mExpiredSessions;

    // Accessed exclusively by the current operation owner, including while it
    // drains deferred closes. No reference escapes that operation.
    // ID순 current-directory 페이지는 송신 시점의 승인 프로필을 읽는다. 삭제된 항목은 다시
    // 나타나지 않고, 페이지 사이의 실제 변경은 전역 profile delta와 같은 작업 순서를 따른다.
    std::map<ServerCore::Session::SessionId, Player> mPlayers;
    std::unordered_map<std::string, ServerCore::Session::SessionId> mNames;
    std::unordered_map<Cell,
        std::unordered_set<ServerCore::Session::SessionId, SessionIdHash>, CellHash> mCells;
    std::size_t mVisibleEdges = 0;
    AoiMetrics mAoiMetrics;
    ServerCore::Session::SessionId mNextControlRecipient = ServerCore::Session::SessionId::Invalid;
    ServerCore::Session::SessionId mNextStateRecipient = ServerCore::Session::SessionId::Invalid;
    std::uint64_t mSchedulerNow = 0;
    std::uint64_t mStateRevision = 0;
    bool mStateBudgetLimited = false;
    bool mControlBudgetLimited = false;
    std::unordered_map<ServerCore::Session::SessionId, PendingJoin, SessionIdHash> mPendingJoins;
};
}
