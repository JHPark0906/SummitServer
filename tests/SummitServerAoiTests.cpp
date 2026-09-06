#include "SummitServerBackend.h"

#include "ServerCore/Core/Clock.h"
#include "ServerCore/Protocol/Framing.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
using Summit::SummitServerBackend;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCore::Protocol::JsonValue;
using ServerCore::Protocol::Message;
using ServerCore::Protocol::MessageFields;
using ServerCore::Session::SessionId;
using ServerCore::Session::SessionState;

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

const JsonValue& Field(const JsonValue& value, std::string_view key)
{
    const auto* field = value.Find(key);
    Check(field != nullptr, "missing AOI field");
    return *field;
}

std::string String(const JsonValue& value, std::string_view key)
{
    const auto* text = Field(value, key).TryString();
    Check(text != nullptr, "expected AOI string");
    return *text;
}

Message Encode(std::string_view type, const JsonValue& body)
{
    const auto encoded = ServerCore::Protocol::SerializeMessage({type, &body, nullptr, nullptr});
    Check(encoded.IsOk(), "fixture serialization failed");
    auto parsed = ServerCore::Protocol::ParseMessage(std::span<const std::byte>(encoded.Value()));
    Check(parsed.IsOk(), "fixture parsing failed");
    return std::move(parsed.Value());
}

class TestSession final : public ServerCore::Session::Session
{
public:
    explicit TestSession(std::uint64_t sessionId) : id(static_cast<SessionId>(sessionId)) {}
    SessionId Id() const noexcept override { return id; }
    SessionState State() const noexcept override { return state; }
    std::size_t QueuedSendBytes() const noexcept override { return queuedSendBytes; }
    Status MarkAuthenticated() override
    {
        if (state != SessionState::Connected) return Status::FailWithoutMessage(ErrorCode::Closed);
        state = SessionState::Authenticated;
        return Status::Ok();
    }
    Status Send(const MessageFields& fields) override
    {
        const auto encoded = ServerCore::Protocol::SerializeMessage(fields);
        Check(encoded.IsOk(), "outgoing AOI serialization failed");
        return RecordSend(encoded.Value());
    }
    Status SendPrepared(const ServerCore::Protocol::PreparedMessage& prepared) override
    {
        // Do not measure the Session fallback's reserialized JSON. These are the exact prepared
        // envelope bytes the real NetworkSession passes to EncodeFrame.
        return RecordSend(prepared.Bytes());
    }
    Status RecordSend(std::span<const std::byte> bytes)
    {
        if (state == SessionState::Closed || state == SessionState::Closing)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        auto parsed = ServerCore::Protocol::ParseMessage(bytes);
        Check(parsed.IsOk(), "outgoing AOI JSON failed to parse");
        const std::string type(parsed.Value().Type());
        if (type == failType)
        {
            failType.clear();
            return Status::FailWithoutMessage(ErrorCode::WouldBlock);
        }
        if (type == callbackType && callback)
        {
            auto action = std::move(callback);
            callbackType.clear();
            action();
        }
        largestFrame = (std::max)(largestFrame, bytes.size());
        Check(bytes.size() <= SummitServerBackend::MaximumBodyBytes, "outgoing frame exceeded 8KiB");
        const auto framed = ServerCore::Protocol::EncodeFrame(bytes, SummitServerBackend::MaximumBodyBytes);
        Check(framed.IsOk() && framed.Value().size() == bytes.size() + ServerCore::Protocol::HeaderSize,
            "recorded AOI wire size omitted the actual frame prefix");
        sentFrames.push_back({type, framed.Value().size()});
        if (type == "VisibilityReady") visibilityReady = true;
        messages.push_back(std::move(parsed.Value()));
        return Status::Ok();
    }
    Status SendAndDisconnect(const MessageFields& fields, Status reason) override
    {
        const Status sent = Send(fields);
        if (sent.IsOk()) { state = SessionState::Closing; error = reason.Code(); }
        return sent;
    }
    void Disconnect(Status reason) override
    {
        if (state == SessionState::Closed) return;
        state = SessionState::Closed;
        error = reason.Code();
        if (onClose) onClose();
    }
    std::size_t Count(std::string_view type) const
    {
        return static_cast<std::size_t>(std::count_if(messages.begin(), messages.end(),
            [&](const Message& message) { return message.Type() == type; }));
    }
    std::vector<JsonValue> Items(std::string_view type, std::string_view key) const
    {
        std::vector<JsonValue> result;
        for (const auto& message : messages)
        {
            if (message.Type() != type) continue;
            const auto* items = Field(*message.Body(), key).TryArray();
            Check(items && items->size() <= SummitServerBackend::MaximumBatchItems, "invalid AOI batch size");
            result.insert(result.end(), items->begin(), items->end());
        }
        return result;
    }
    std::size_t WireBytes(std::string_view type = {}) const
    {
        std::size_t bytes = 0;
        for (const auto& frame : sentFrames)
            if (type.empty() || frame.type == type) bytes += frame.bytes;
        return bytes;
    }
    void Clear() { messages.clear(); sentFrames.clear(); read = 0; }

    struct SentFrame { std::string type; std::size_t bytes; };
    SessionId id;
    SessionState state = SessionState::Connected;
    ErrorCode error = ErrorCode::Ok;
    std::vector<Message> messages;
    std::vector<SentFrame> sentFrames;
    std::size_t read = 0, largestFrame = 0;
    std::size_t queuedSendBytes = 0;
    bool directoryReady = false, hasPosition = false, visibilityReady = false;
    std::string failType, callbackType;
    std::function<void()> callback, onClose;
};

struct Fixture
{
    explicit Fixture(std::size_t capacity = 128, SummitServerBackend::SchedulerOptions configured = {})
        : options(configured), backend(std::make_shared<SummitServerBackend>(capacity, capacity, configured)),
          now(ServerCore::Core::MillisecondsSinceProcessStart())
    {
        Check(backend->RegisterHandlers(dispatcher).IsOk(), "AOI handlers did not register");
        dispatcher.Freeze();
    }
    Status Dispatch(const std::shared_ptr<TestSession>& session, std::string_view type, const JsonValue& body)
    {
        return dispatcher.Dispatch(session, Encode(type, body));
    }
    std::shared_ptr<TestSession> Join(std::string name)
    {
        auto session = std::make_shared<TestSession>(++nextId);
        const auto weakBackend = std::weak_ptr<SummitServerBackend>(backend);
        const SessionId id = session->Id();
        session->onClose = [weakBackend, id]() {
            if (const auto owner = weakBackend.lock())
                owner->OnSessionClosed(id, Status::FailWithoutMessage(ErrorCode::Closed));
        };
        sessions.push_back(session);
        backend->OnSessionOpened(session);
        JsonValue::Object fields;
        fields.emplace("schemaVersion", JsonValue(SummitServerBackend::SchemaVersion));
        fields.emplace("name", JsonValue(std::move(name)));
        fields.emplace("c", JsonValue(0));
        Check(Dispatch(session, "Join", JsonValue(std::move(fields))).IsOk(), "AOI Join failed");
        Check(session->state == SessionState::Authenticated, "AOI Join rejected a valid profile");
        return session;
    }
    Status State(const std::shared_ptr<TestSession>& session, double x, double y,
        std::uint64_t q = 0, bool omitQ = false, bool escapedAnimation = false,
        double velocityX = 1.0, std::string animation = "walk", double facing = -1.0)
    {
        JsonValue::Object fields;
        fields.emplace("x", JsonValue(x)); fields.emplace("y", JsonValue(y));
        fields.emplace("vx", JsonValue(escapedAnimation ? 3.0e38 : velocityX));
        fields.emplace("vy", JsonValue(escapedAnimation ? -3.0e38 : 0.0));
        fields.emplace("f", JsonValue(facing));
        fields.emplace("s", JsonValue(escapedAnimation ? std::string(32, '\0') : std::move(animation)));
        fields.emplace("c", JsonValue(5));
        if (!omitQ) fields.emplace("q", JsonValue(q));
        const auto sent = Dispatch(session, "PlayerState", JsonValue(std::move(fields)));
        if (sent.IsOk() && session->state == SessionState::Authenticated) session->hasPosition = true;
        return sent;
    }
    static std::size_t ControlBytes(const TestSession& session)
    {
        return session.WireBytes("DirectoryPage") + session.WireBytes("DirectoryReady") +
            session.WireBytes("VisibilityEnter") + session.WireBytes("VisibilityExit") +
            session.WireBytes("VisibilityReady");
    }
    void Tick()
    {
        const auto beforeMetrics = backend->SnapshotAoiMetrics();
        std::vector<std::pair<std::size_t, std::size_t>> before;
        for (const auto& session : sessions)
            before.emplace_back(session->WireBytes("StateBatch"), ControlBytes(*session));
        now += SummitServerBackend::ReplicationIntervalMilliseconds;
        backend->Tick(now);
        lastStateBytes = lastControlBytes = 0;
        for (std::size_t index = 0; index < before.size(); ++index)
        {
            const std::size_t stateBytes = sessions[index]->WireBytes("StateBatch") - before[index].first;
            const std::size_t controlBytes = ControlBytes(*sessions[index]) - before[index].second;
            Check(stateBytes <= options.perClientStateBytesPerTick, "per-client state wire-byte budget exceeded");
            Check(controlBytes <= options.perClientControlBytesPerTick, "per-client control wire-byte budget exceeded");
            lastStateBytes += stateBytes;
            lastControlBytes += controlBytes;
        }
        Check(lastStateBytes <= options.globalStateBytesPerTick, "global state wire-byte budget exceeded");
        Check(lastControlBytes <= options.globalControlBytesPerTick, "global control wire-byte budget exceeded");
        const auto afterMetrics = backend->SnapshotAoiMetrics();
        Check(afterMetrics.lastTickStateBytes == lastStateBytes && afterMetrics.lastTickControlBytes == lastControlBytes &&
            afterMetrics.sentStateBytes - beforeMetrics.sentStateBytes == lastStateBytes &&
            afterMetrics.sentControlBytes - beforeMetrics.sentControlBytes == lastControlBytes,
            "scheduler byte metrics disagree with actual successful framed output");
    }
    void Ack(const std::shared_ptr<TestSession>& session, std::string cursor)
    {
        JsonValue::Object fields;
        fields.emplace("cursor", JsonValue(std::move(cursor)));
        Check(Dispatch(session, "DirectoryAck", JsonValue(std::move(fields))).IsOk(), "valid directory ACK failed");
    }
    void Sync()
    {
        for (unsigned turn = 0; turn < 256; ++turn)
        {
            Tick();
            bool allReady = true;
            for (const auto& session : sessions)
            {
                if (session->state != SessionState::Authenticated) continue;
                while (session->read < session->messages.size())
                {
                    const auto& message = session->messages[session->read++];
                    if (message.Type() == "DirectoryPage") Ack(session, String(*message.Body(), "cursor"));
                    else if (message.Type() == "DirectoryReady") session->directoryReady = true;
                }
                allReady &= session->directoryReady && (!session->hasPosition || session->visibilityReady);
            }
            if (allReady) return;
        }
        throw std::runtime_error("directory did not finish within bounded page rounds");
    }
    void Clear() { for (const auto& session : sessions) session->Clear(); }
    SummitServerBackend::SchedulerOptions options;
    std::shared_ptr<SummitServerBackend> backend;
    ServerCore::Dispatch::Dispatcher dispatcher;
    std::vector<std::shared_ptr<TestSession>> sessions;
    std::uint64_t now, nextId = 0;
    std::size_t lastStateBytes = 0, lastControlBytes = 0;
};

std::string WireId(const std::shared_ptr<TestSession>& session)
{
    return std::to_string(static_cast<std::uint64_t>(session->Id()));
}

struct VisibilityLedger
{
    void Apply(const TestSession& receiver)
    {
        for (const auto& message : receiver.messages)
        {
            if (message.Type() == "VisibilityEnter")
            {
                for (const auto& item : *Field(*message.Body(), "players").TryArray())
                {
                    const auto id = String(item, "id");
                    Check(!visible.contains(id), "successful enter duplicated an already visible entity");
                    visible.emplace(id, *Field(item, "q").TryUInt64());
                }
            }
            else if (message.Type() == "StateBatch")
            {
                for (const auto& item : *Field(*message.Body(), "states").TryArray())
                {
                    const auto id = String(item, "id");
                    const auto found = visible.find(id);
                    Check(found != visible.end(), "state preceded its successful enter or followed exit/leave");
                    const auto q = *Field(item, "q").TryUInt64();
                    Check(q >= found->second, "state revision regressed to an older queued snapshot");
                    found->second = q;
                }
            }
            else if (message.Type() == "VisibilityExit")
            {
                for (const auto& id : *Field(*message.Body(), "ids").TryArray())
                    Check(visible.erase(*id.TryString()) == 1, "exit preceded enter or removed an entity twice");
            }
            else if (message.Type() == "PlayerLeft")
                visible.erase(String(*message.Body(), "id"));
        }
    }
    std::unordered_map<std::string, std::uint64_t> visible;
};

void TestSchedulerWireBudgetsAndLatestState()
{
    auto options = SummitServerBackend::SchedulerOptions{};
    options.perClientStateBytesPerTick = 1024;
    options.globalStateBytesPerTick = 16u * 1024u * 1024u;
    options.perClientControlBytesPerTick = 1024;
    options.globalControlBytesPerTick = 4096;
    Fixture fixture(32, options);
    const auto viewer = fixture.Join("BudgetViewer");
    std::vector<std::shared_ptr<TestSession>> entities;
    for (unsigned index = 0; index < 16; ++index)
    {
        const auto entity = fixture.Join(std::string(44, '"') + std::to_string(index));
        entities.push_back(entity);
        Check(fixture.State(entity, 1, 1, 1, false, true).IsOk(), "budget fixture state failed");
    }
    Check(fixture.State(viewer, 0, 0).IsOk(), "budget observer state failed");
    fixture.Sync();
    Check(viewer->Items("VisibilityEnter", "players").size() == entities.size() &&
        viewer->Count("VisibilityEnter") > 1 && viewer->Count("VisibilityReady") == 1,
        "small control budgets lost initial entities or failed to split the initial visibility set");
    bool ready = false;
    for (const auto& message : viewer->messages)
    {
        if (message.Type() == "VisibilityEnter") Check(!ready, "VisibilityReady preceded the final initial enter");
        if (message.Type() == "VisibilityReady") ready = true;
    }
    VisibilityLedger ledger;
    ledger.Apply(*viewer);
    for (std::uint64_t round = 0; round < 3; ++round)
    {
        fixture.Clear();
        const std::uint64_t latest = 101 + round * 2;
        for (const auto& entity : entities)
        {
            Check(fixture.State(entity, 2, 1, latest - 1, false, true).IsOk(), "intermediate state failed");
            Check(fixture.State(entity, 3, 1, latest, false, true).IsOk(), "latest replacement state failed");
        }
        fixture.Tick();
        const auto states = viewer->Items("StateBatch", "states");
        Check(!states.empty() && states.size() < entities.size(), "tight state budget did not cause real deferral");
        for (const auto& state : states)
            Check(*Field(state, "q").TryUInt64() == latest, "budget deferral replayed a stale state FIFO");
        ledger.Apply(*viewer);
    }
    // Stop every target and then send no further inputs: their final revision must still drain.
    for (const auto& entity : entities)
        Check(fixture.State(entity, 3, 1, 1000, false, false, 0.0, std::string(32, '\0')).IsOk(),
            "final stationary state failed");
    bool allFinal = false;
    for (unsigned turn = 0; turn < 64 && !allFinal; ++turn)
    {
        fixture.Clear(); fixture.Tick();
        for (const auto& state : viewer->Items("StateBatch", "states"))
            Check(*Field(state, "q").TryUInt64() == 1000, "a deferred pre-stop snapshot escaped after its replacement");
        ledger.Apply(*viewer);
        allFinal = std::all_of(entities.begin(), entities.end(), [&](const auto& entity)
        {
            return ledger.visible.at(WireId(entity)) == 1000;
        });
    }
    Check(allFinal, "budget scheduler starved final states after their senders stopped producing inputs");
    fixture.Clear(); fixture.Tick();
    Check(viewer->Count("StateBatch") == 0, "successful final revisions stayed dirty and were sent twice");
}

void TestSchedulerAgeFairness()
{
    auto options = SummitServerBackend::SchedulerOptions{};
    options.perClientStateBytesPerTick = 1024;
    options.globalStateBytesPerTick = 16u * 1024u * 1024u;
    Fixture fixture(32, options);
    const auto viewer = fixture.Join("AgeViewer");
    std::vector<std::shared_ptr<TestSession>> nearby;
    for (unsigned index = 0; index < 12; ++index)
    {
        const auto entity = fixture.Join("Nearby" + std::to_string(index));
        nearby.push_back(entity);
        Check(fixture.State(entity, 1, 1, 1, false, false, 0.0, std::string(32, '\0')).IsOk(),
            "nearby age fixture failed");
    }
    const auto distant = fixture.Join("Distant");
    Check(fixture.State(distant, 31, 19, 1, false, false, 0.0, std::string(32, '\0')).IsOk() &&
        fixture.State(viewer, 0, 0).IsOk(), "distant age fixture failed");
    fixture.Sync();
    Check(fixture.State(distant, 31, 19, 1000, false, false, 0.0, std::string(32, '\0')).IsOk(),
        "stationary distant revision failed");
    bool delivered = false;
    for (unsigned turn = 0; turn < 64 && !delivered; ++turn)
    {
        fixture.Clear();
        for (const auto& entity : nearby)
            Check(fixture.State(entity, turn % 2 == 0 ? 2 : 1, 1, 100 + turn, false, false,
                0.0, std::string(32, '\0')).IsOk(),
                "continuous nearby movement failed");
        fixture.Tick();
        for (const auto& state : viewer->Items("StateBatch", "states"))
            if (String(state, "id") == WireId(distant))
            {
                Check(*Field(state, "q").TryUInt64() == 1000, "distant heartbeat arrived with a stale revision");
                Check(turn != 0, "test did not exercise priority deferral before age overcame nearby movement");
                delivered = true;
            }
    }
    Check(delivered, "continuous nearby changes starved an older distant state beyond bounded priority bonuses");
}

void TestSchedulerGlobalRoundRobin()
{
    auto options = SummitServerBackend::SchedulerOptions{};
    options.perClientStateBytesPerTick = 1024;
    options.globalStateBytesPerTick = 1024;
    Fixture fixture(16, options);
    for (unsigned index = 0; index < 8; ++index)
    {
        const auto entity = fixture.Join("Receiver" + std::to_string(index));
        Check(fixture.State(entity, 0, 0, 1).IsOk(), "round-robin fixture failed");
    }
    fixture.Sync();
    std::unordered_set<std::string> served;
    for (unsigned turn = 0; turn < 32; ++turn)
    {
        fixture.Clear();
        for (const auto& entity : fixture.sessions)
            Check(fixture.State(entity, 0, 0, 100 + turn).IsOk(), "round-robin heartbeat failed");
        fixture.Tick();
        std::size_t servedThisTick = 0;
        for (const auto& receiver : fixture.sessions)
        {
            const auto states = receiver->Items("StateBatch", "states");
            if (!states.empty()) { served.insert(WireId(receiver)); ++servedThisTick; }
            for (const auto& state : states)
                Check(*Field(state, "q").TryUInt64() == 100 + turn,
                    "global budget retained old payloads instead of choosing current state");
        }
        if (turn == 0)
            Check(servedThisTick > 0 && servedThisTick < fixture.sessions.size(),
                "global budget fixture did not actually defer any recipient");
    }
    Check(served.size() == fixture.sessions.size(), "global state budget always favored the same receiver IDs");
}

void TestSchedulerQueueBacklog()
{
    Fixture fixture;
    const auto viewer = fixture.Join("BacklogViewer");
    const auto entity = fixture.Join("BacklogTarget");
    Check(fixture.State(viewer, 0, 0).IsOk() && fixture.State(entity, 1, 1, 1).IsOk(),
        "backlog fixture failed");
    fixture.Sync(); fixture.Clear();
    viewer->queuedSendBytes = 8192;
    Check(fixture.State(entity, 2, 1, 2, false, false, 0.0, "idle").IsOk(), "backlogged state failed");
    fixture.Tick();
    Check(viewer->state == SessionState::Authenticated && viewer->Count("StateBatch") == 0,
        "state scheduler added stale work to a full transport backlog");
    viewer->queuedSendBytes = 0;
    fixture.Tick();
    const auto resumed = viewer->Items("StateBatch", "states");
    Check(resumed.size() == 1 && *Field(resumed[0], "q").TryUInt64() == 2,
        "backlog deferral advanced revision and lost a stopped target's final pose");
    fixture.Clear();
    viewer->queuedSendBytes = (std::numeric_limits<std::size_t>::max)();
    Check(fixture.State(entity, 3, 1, 3).IsOk(), "overflow backlog fixture failed"); fixture.Tick();
    Check(viewer->Count("StateBatch") == 0, "backlog plus frame size overflowed the transport admission check");
    viewer->queuedSendBytes = 0;
    Check(fixture.State(entity, 4, 1, 4).IsOk(), "latest backlog replacement failed"); fixture.Tick();
    const auto latest = viewer->Items("StateBatch", "states");
    Check(latest.size() == 1 && *Field(latest[0], "q").TryUInt64() == 4,
        "transport recovery retried an obsolete state or skipped its replacement");
}

void TestSchedulerDeferredVisibilityLifetime()
{
    auto options = SummitServerBackend::SchedulerOptions{};
    options.perClientControlBytesPerTick = 1024;
    options.globalControlBytesPerTick = 8192;
    Fixture fixture(16, options);
    const auto viewer = fixture.Join("LifetimeViewer");
    std::vector<std::shared_ptr<TestSession>> entities;
    for (unsigned index = 0; index < 8; ++index)
        entities.push_back(fixture.Join(std::string(44, '"') + std::to_string(index)));
    fixture.Sync(); fixture.Clear();
    Check(fixture.State(viewer, 0, 0).IsOk(), "lifetime observer fixture failed");
    for (const auto& entity : entities)
        Check(fixture.State(entity, 1, 1, 1, false, true).IsOk(), "lifetime entity fixture failed");
    fixture.Tick();
    VisibilityLedger ledger;
    ledger.Apply(*viewer);
    Check(!ledger.visible.empty() && ledger.visible.size() < entities.size() && viewer->Count("VisibilityReady") == 0,
        "lifetime fixture did not produce a partial initial visibility set");
    std::shared_ptr<TestSession> entered, deferred;
    for (const auto& entity : entities)
    {
        if (ledger.visible.contains(WireId(entity))) entered = entity;
        else deferred = entity;
    }
    Check(entered && deferred, "lifetime fixture did not retain both accepted and deferred enters");
    fixture.Clear();
    Check(fixture.State(entered, 200, 200, 3).IsOk() && fixture.State(deferred, 200, 200, 3).IsOk(),
        "teleporting accepted/deferred entities failed");
    viewer->failType = "VisibilityExit";
    fixture.Tick();
    Check(viewer->state == SessionState::Authenticated && viewer->Count("VisibilityExit") == 0,
        "recoverable exit pressure disconnected or falsely committed the exit");
    for (const auto& state : viewer->Items("StateBatch", "states"))
        Check(String(state, "id") != WireId(entered), "state was sent to an entity already outside the AOI while its exit waited");
    ledger.Apply(*viewer);
    Check(ledger.visible.contains(WireId(entered)) && !ledger.visible.contains(WireId(deferred)),
        "failed exit removed a relation or a deferred enter replayed its obsolete desired set");
    fixture.Clear(); fixture.Tick(); ledger.Apply(*viewer);
    Check(!ledger.visible.contains(WireId(entered)), "successful exit retry did not remove the relation");
    fixture.Clear(); fixture.Sync(); ledger.Apply(*viewer);
    Check(ledger.visible.size() == entities.size() - 2 && !ledger.visible.contains(WireId(deferred)) &&
        viewer->visibilityReady, "partial initial sync resurrected a target that teleported before its enter was sent");

    fixture.Clear();
    viewer->failType = "VisibilityEnter";
    Check(fixture.State(deferred, 1, 1, 10).IsOk(), "returning deferred target failed"); fixture.Tick();
    Check(viewer->Count("VisibilityEnter") == 0, "recoverable enter unexpectedly succeeded");
    for (const auto& state : viewer->Items("StateBatch", "states"))
        Check(String(state, "id") != WireId(deferred), "state preceded a backpressured reentry");
    ledger.Apply(*viewer);
    fixture.Clear();
    viewer->failType = "VisibilityEnter";
    fixture.Tick();
    Check(viewer->failType.empty() && viewer->Count("VisibilityEnter") == 0 &&
        viewer->state == SessionState::Authenticated,
        "an unchanged pending enter was forgotten instead of retried after WouldBlock");
    ledger.Apply(*viewer);
    fixture.Clear();
    Check(fixture.State(deferred, 2, 1, 11).IsOk(), "latest reentry replacement failed"); fixture.Tick();
    const auto reentered = viewer->Items("VisibilityEnter", "players");
    Check(reentered.size() == 1 && String(reentered[0], "id") == WireId(deferred) &&
        *Field(reentered[0], "q").TryUInt64() == 11,
        "deferred reentry reused an obsolete full pose or failed to recreate the latest relation");
    ledger.Apply(*viewer);
    fixture.Clear();
    viewer->queuedSendBytes = 8192;
    Check(fixture.State(deferred, 3, 1, 12).IsOk(), "departing dirty target failed"); fixture.Tick();
    deferred->Disconnect(Status::Ok());
    ledger.Apply(*viewer);
    Check(viewer->Count("PlayerLeft") == 1 && !ledger.visible.contains(WireId(deferred)),
        "global leave did not clear a target with a deferred state");
    fixture.Clear(); viewer->queuedSendBytes = 0; fixture.Tick(); ledger.Apply(*viewer);
    Check(viewer->Count("StateBatch") == 0 && viewer->Count("VisibilityExit") == 0,
        "a departed deferred target generated a stale state or redundant AOI exit");
}

void TestSchedulerReceiverClosesDuringSend()
{
    Fixture fixture;
    const auto viewer = fixture.Join("ClosingViewer");
    const auto entity = fixture.Join("RemainingTarget");
    Check(fixture.State(viewer, 0, 0).IsOk() && fixture.State(entity, 1, 1, 1).IsOk(),
        "receiver close fixture failed");
    fixture.Sync(); fixture.Clear();
    viewer->callbackType = "StateBatch";
    viewer->callback = [&]() { viewer->Disconnect(Status::Ok()); };
    Check(fixture.State(entity, 2, 1, 2).IsOk(), "reentrant receiver update failed"); fixture.Tick();
    const auto metrics = fixture.backend->SnapshotAoiMetrics();
    Check(viewer->state == SessionState::Closed && metrics.joinedPlayers == 1 && metrics.positionedPlayers == 1 &&
        metrics.visibleEdges == 0, "receiver close during scheduled send left owned relations or iterator state behind");
    fixture.Clear(); fixture.Tick();
    Check(entity->state == SessionState::Authenticated && entity->Count("StateBatch") == 0 &&
        entity->Count("VisibilityExit") == 0, "closed receiver remained in a subsequent scheduler round");
}

void TestDirectoryWindowAndLiveProfileChanges()
{
    auto options = SummitServerBackend::SchedulerOptions{};
    // Isolate the existing ACK/page-size contract from global scheduling; separate tests below
    // deliberately exhaust that budget and inspect the exact wire bytes of every tick.
    options.globalControlBytesPerTick = 16u * 1024u * 1024u;
    Fixture fixture(128, options);
    for (unsigned index = 0; index < 80; ++index)
        fixture.Join(std::string(44, '"') + std::to_string(index));
    const auto viewer = fixture.sessions.back();
    fixture.Tick();
    Check(viewer->Count("DirectoryPage") == 1 && viewer->Count("DirectoryReady") == 0,
        "directory did not start with one outstanding page");
    fixture.Tick(); fixture.Tick();
    Check(viewer->Count("DirectoryPage") == 1, "server sent a second page without ACK");
    const auto firstPage = viewer->Items("DirectoryPage", "players");
    Check(firstPage.size() == 32, "ordinary maximum-length profile page did not retain its item budget");
    const auto departed = fixture.sessions[50];
    const std::string departedId = std::to_string(static_cast<std::uint64_t>(departed->Id()));
    departed->Disconnect(Status::Ok());
    const auto renamed = fixture.sessions[51];
    JsonValue::Object profile;
    profile.emplace("name", JsonValue(std::string("페이지 전 변경")));
    profile.emplace("c", JsonValue(4));
    Check(fixture.Dispatch(renamed, "SetProfile", JsonValue(std::move(profile))).IsOk(), "live profile update failed");
    fixture.Join("snapshot 이후 입장");
    fixture.Sync();
    const auto all = viewer->Items("DirectoryPage", "players");
    Check(all.size() == 78, "paged directory duplicated, skipped, or included a post-boundary identity");
    bool sawRenamed = false;
    for (const auto& item : all)
    {
        Check(String(item, "id") != departedId, "later page resurrected a departed identity");
        if (String(item, "id") == std::to_string(static_cast<std::uint64_t>(renamed->Id())))
            sawRenamed = String(item, "name") == "페이지 전 변경" && *Field(item, "c").TryUInt64() == 4;
    }
    Check(sawRenamed && viewer->Count("PlayerJoined") == 1 && viewer->Count("DirectoryReady") == 1,
        "latest profile or concurrent post-boundary join was missing");
    Check(viewer->largestFrame <= SummitServerBackend::MaximumBodyBytes, "paged roster exceeded frame limit");
}

void TestSpatialBoundariesCoalescingAndReentry()
{
    Fixture fixture;
    auto viewer = fixture.Join("Viewer");
    auto moving = fixture.Join("Moving");
    auto far = fixture.Join("Far");
    fixture.Sync(); fixture.Clear();
    Check(viewer->Count("VisibilityReady") == 0, "positionless player received a fabricated visibility set");
    Check(fixture.State(viewer, -16, -16).IsOk() && fixture.State(moving, 16, 4, 1).IsOk() &&
        fixture.State(far, 500, 500, 2).IsOk(), "valid position failed");
    Check(viewer->Count("VisibilityEnter") == 0, "state was immediately broadcast outside the tick");
    fixture.Tick();
    auto entered = viewer->Items("VisibilityEnter", "players");
    Check(entered.size() == 1 && String(entered[0], "name") == "Moving" &&
        *Field(entered[0], "c").TryUInt64() == 0 && *Field(entered[0], "q").TryUInt64() == 1,
        "inclusive negative-cell boundary, full initial state, or approved character failed");
    Check(viewer->Count("VisibilityReady") == 1 && far->Count("VisibilityReady") == 1 &&
        far->Items("VisibilityEnter", "players").empty(), "initial empty/nonempty visibility did not finish");
    fixture.Clear();
    Check(fixture.State(moving, 18, 6, 2).IsOk() && fixture.State(moving, 20, 8, 3).IsOk(), "movement failed");
    fixture.Tick();
    auto states = viewer->Items("StateBatch", "states");
    Check(states.size() == 1 && *Field(states[0], "q").TryUInt64() == 3 &&
        viewer->Count("VisibilityExit") == 0 && fixture.backend->SnapshotAoiMetrics().coalescedStates >= 1,
        "hysteresis boundary or latest-state coalescing failed");
    fixture.Clear();
    Check(fixture.State(moving, 20.01, 8, 4).IsOk(), "exit movement failed"); fixture.Tick();
    Check(viewer->Items("VisibilityExit", "ids").size() == 1 && viewer->Count("StateBatch") == 0,
        "exit left a stale visible entity or sent state after exit");
    fixture.Clear();
    Check(fixture.State(moving, 18, 6, 5).IsOk(), "outer-band movement failed"); fixture.Tick();
    Check(viewer->Count("VisibilityEnter") == 0 && viewer->Count("StateBatch") == 0,
        "hysteresis outer band recreated an exited entity");
    Check(fixture.State(moving, 16, 4, (std::numeric_limits<std::uint64_t>::max)()).IsOk(), "reentry failed");
    fixture.Tick();
    entered = viewer->Items("VisibilityEnter", "players");
    Check(entered.size() == 1 && *Field(entered[0], "q").TryUInt64() ==
        (std::numeric_limits<std::uint64_t>::max)(), "reentry did not include exact latest sequence and full pose");
    fixture.Clear();
    Check(fixture.State(viewer, -500, -500).IsOk(), "observer teleport failed"); fixture.Tick();
    Check(viewer->Items("VisibilityExit", "ids").size() == 1,
        "moving the observer did not recompute outgoing visibility");
    JsonValue::Object chat; chat.emplace("text", JsonValue(std::string("먼 곳 전체 채팅")));
    Check(fixture.Dispatch(far, "Chat", JsonValue(std::move(chat))).IsOk() && viewer->Count("ChatMessage") == 1,
        "AOI filtered global chat");
    Check(fixture.backend->Announce("전체 공지").IsOk(), "AOI blocked a global announcement");
    Check(fixture.backend->SnapshotAoiMetrics().visibleEdges == 0, "teleport retained stale visibility edges");
}

void TestStationaryMovementAndProfileReplication()
{
    Fixture fixture;
    const auto viewer = fixture.Join("StationaryViewer");
    const auto target = fixture.Join("StationaryTarget");
    const auto distant = fixture.Join("StationaryDistant");
    Check(fixture.State(viewer, 0, 0).IsOk() && fixture.State(target, 1, 1, 1).IsOk() &&
        fixture.State(distant, 200, 0).IsOk(), "stationary profile fixture failed");
    fixture.Sync();
    const auto baseline = viewer->Items("VisibilityEnter", "players");
    Check(baseline.size() == 1, "stationary viewer did not receive exactly its nearby target");
    auto revision = std::stoull(String(baseline.front(), "r"));
    fixture.Clear();
    Check(fixture.State(target, 1, 1, 2).IsOk(), "stationary q-only state failed"); fixture.Tick();
    auto states = viewer->Items("StateBatch", "states");
    Check(states.size() == 1 && *Field(states.front(), "q").TryUInt64() == 2 &&
        std::stoull(String(states.front(), "r")) > revision,
        "stationary q-only input was lost when no visibility change was needed");
    revision = std::stoull(String(states.front(), "r"));
    fixture.Clear();
    Check(fixture.State(target, 1, 1, 3, false, false, 0, "idle", 1).IsOk(),
        "stationary stop/facing update failed"); fixture.Tick();
    states = viewer->Items("StateBatch", "states");
    Check(states.size() == 1 && *Field(states.front(), "q").TryUInt64() == 3 &&
        *Field(states.front(), "vx").TryNumber() == 0 && *Field(states.front(), "f").TryNumber() == 1 &&
        String(states.front(), "s") == "idle" && std::stoull(String(states.front(), "r")) > revision,
        "unchanged coordinates suppressed the final velocity, facing, animation or revision");
    revision = std::stoull(String(states.front(), "r"));
    Check(viewer->Count("VisibilityEnter") == 0 && viewer->Count("VisibilityExit") == 0 &&
        distant->Count("StateBatch") == 0, "stationary pose update altered spatial membership");
    fixture.Clear();
    Check(fixture.Dispatch(target, "SetProfile", JsonValue(JsonValue::Object{
        {"name", JsonValue(std::string("RenamedStationary"))}, {"c", JsonValue(4)}})).IsOk(),
        "stationary visible profile change failed");
    for (const auto& session : fixture.sessions)
        Check(session->Count("ProfileChanged") == 1, "approved profile did not reach all joined players");
    fixture.Tick();
    states = viewer->Items("StateBatch", "states");
    Check(states.size() == 1 && *Field(states.front(), "q").TryUInt64() == 3 &&
        *Field(states.front(), "c").TryUInt64() == 4 && std::stoull(String(states.front(), "r")) > revision,
        "stationary profile change left the old prepared appearance or revision cached");
    Check(viewer->Count("VisibilityEnter") == 0 && viewer->Count("VisibilityExit") == 0 &&
        distant->Count("StateBatch") == 0, "profile change recreated visibility or leaked distant movement");
    fixture.Clear(); fixture.Tick();
    Check(viewer->Count("StateBatch") == 0 && viewer->Count("VisibilityEnter") == 0 && viewer->Count("VisibilityExit") == 0,
        "quiet TCP state replayed without a new revision");
}

void TestMovingViewerWithinCellAndTeleportNeighborhoods()
{
    Fixture fixture;
    const auto moving = fixture.Join("NeighborhoodTraveler");
    const auto oldNeighbor = fixture.Join("NeighborhoodOld");
    const auto newNeighbor = fixture.Join("NeighborhoodNew");
    Check(fixture.State(moving, 7, 1, 1).IsOk() && fixture.State(oldNeighbor, 40, 1, 1).IsOk() &&
        fixture.State(newNeighbor, -200, -200, 1).IsOk(), "neighborhood fixture failed");
    fixture.Sync();
    std::vector<VisibilityLedger> ledgers(fixture.sessions.size());
    const auto consume = [&]() {
        for (std::size_t index = 0; index < ledgers.size(); ++index) ledgers[index].Apply(*fixture.sessions[index]);
        fixture.Clear();
    };
    consume();
    Check(ledgers[0].visible.empty() && ledgers[1].visible.empty(), "outer band created initial visibility");
    // All three observer positions 8/5/3 remain in cell x=0. A cell-key-only
    // invalidation would miss both its outgoing set and the stationary observer.
    Check(fixture.State(moving, 8, 1, 2).IsOk(), "same-cell entering observer failed"); fixture.Tick(); consume();
    Check(ledgers[0].visible.contains(WireId(oldNeighbor)) && ledgers[1].visible.contains(WireId(moving)),
        "same-cell viewer movement did not enter in both viewing directions");
    Check(fixture.State(moving, 5, 1, 3).IsOk(), "same-cell hysteresis observer failed"); fixture.Tick(); consume();
    Check(ledgers[0].visible.contains(WireId(oldNeighbor)) && ledgers[1].visible.contains(WireId(moving)),
        "same-cell observer movement forgot hysteresis membership");
    Check(fixture.State(moving, 3, 1, 4).IsOk(), "same-cell exiting observer failed"); fixture.Tick(); consume();
    Check(ledgers[0].visible.empty() && ledgers[1].visible.empty(),
        "same-cell observer movement retained an out-of-range relation");
    Check(fixture.State(moving, 8, 1, 5).IsOk(), "old neighborhood restoration failed"); fixture.Tick(); consume();
    Check(fixture.State(moving, -199, -200, 6).IsOk(), "old-to-new neighborhood teleport failed"); fixture.Tick(); consume();
    Check(ledgers[0].visible.size() == 1 && ledgers[0].visible.contains(WireId(newNeighbor)) &&
        ledgers[1].visible.empty() && ledgers[2].visible.size() == 1 && ledgers[2].visible.contains(WireId(moving)),
        "teleport did not update traveler, old observers and new observers together");
    Check(fixture.State(moving, 8, 1, 7).IsOk(), "return neighborhood teleport failed"); fixture.Tick(); consume();
    Check(ledgers[0].visible.size() == 1 && ledgers[0].visible.contains(WireId(oldNeighbor)) &&
        ledgers[1].visible.size() == 1 && ledgers[1].visible.contains(WireId(moving)) && ledgers[2].visible.empty(),
        "return teleport retained new-neighborhood state or missed its prior stationary observer");
}

void TestBatchBoundsAndStateBackpressure()
{
    Fixture fixture;
    auto viewer = fixture.Join("Observer");
    for (unsigned index = 0; index < 40; ++index)
    {
        const auto entity = fixture.Join("Entity" + std::to_string(index));
        Check(fixture.State(entity, 1, 1, index, false, true).IsOk(), "escaped animation fixture failed");
    }
    Check(fixture.State(viewer, 0, 0).IsOk(), "observer fixture failed");
    fixture.Sync();
    Check(viewer->Items("VisibilityEnter", "players").size() == 40 && viewer->Count("VisibilityEnter") >= 2,
        "large initial visibility was not fully delivered in bounded batches");
    fixture.Clear();
    auto entity = fixture.sessions[1];
    viewer->failType = "StateBatch";
    Check(fixture.State(entity, 2, 1, 100).IsOk(), "updated state failed"); fixture.Tick();
    Check(viewer->state == SessionState::Authenticated && viewer->Count("StateBatch") == 0,
        "state queue backpressure disconnected a recoverable receiver");
    fixture.Tick();
    const auto stoppedRetry = viewer->Items("StateBatch", "states");
    Check(stoppedRetry.size() == 1 && *Field(stoppedRetry[0], "q").TryUInt64() == 100,
        "a failed state send advanced revision and lost a target that stopped sending inputs");
    fixture.Clear();
    viewer->failType = "StateBatch";
    Check(fixture.State(entity, 2, 1, 101).IsOk(), "second backpressure state failed"); fixture.Tick();
    Check(viewer->Count("StateBatch") == 0, "second recoverable send unexpectedly succeeded");
    Check(fixture.State(entity, 3, 1, 102).IsOk(), "replacement state failed"); fixture.Tick();
    const auto states = viewer->Items("StateBatch", "states");
    Check(states.size() == 1 && *Field(states[0], "q").TryUInt64() == 102,
        "failed state send advanced revision or retried an obsolete snapshot");
    fixture.Clear();
    viewer->callbackType = "StateBatch";
    viewer->callback = [&]() { entity->Disconnect(Status::Ok()); };
    Check(fixture.State(entity, 4, 1, 103).IsOk(), "reentrant state fixture failed"); fixture.Tick();
    Check(viewer->Count("StateBatch") == 1 && viewer->Count("PlayerLeft") == 1,
        "reentrant close aborted replication or lost global removal");
    std::size_t stateAt = viewer->messages.size(), leftAt = viewer->messages.size();
    for (std::size_t index = 0; index < viewer->messages.size(); ++index)
    {
        if (viewer->messages[index].Type() == "StateBatch") stateAt = index;
        if (viewer->messages[index].Type() == "PlayerLeft") leftAt = index;
    }
    Check(stateAt < leftAt, "close overtook the in-flight state batch");
    fixture.Clear(); fixture.Tick();
    Check(viewer->Count("VisibilityExit") == 0 && viewer->Count("StateBatch") == 0,
        "closed entity remained indexed or generated a redundant AOI exit");
}

void TestSynchronizationErrorsAndCoordinateLimits()
{
    {
        Fixture fixture;
        const auto pending = std::make_shared<TestSession>(1);
        fixture.backend->OnSessionOpened(pending);
        JsonValue::Object ack; ack.emplace("cursor", JsonValue(std::string("1")));
        Check(fixture.Dispatch(pending, "DirectoryAck", JsonValue(std::move(ack))).IsOk() &&
            pending->state == SessionState::Closing && pending->Count("JoinRejected") == 1,
            "pre-join ACK bypassed the authentication gate");
    }
    for (const std::string cursor : {"0", "01", "-1", "18446744073709551616"})
    {
        Fixture fixture;
        fixture.Join("Existing"); auto viewer = fixture.Join("Viewer"); fixture.Tick();
        JsonValue::Object ack; ack.emplace("cursor", JsonValue(cursor));
        Check(fixture.Dispatch(viewer, "DirectoryAck", JsonValue(std::move(ack))).Code() == ErrorCode::InvalidFormat &&
            viewer->state == SessionState::Closed, "incorrect directory ACK was accepted");
    }
    {
        Fixture fixture; fixture.Join("Existing"); auto viewer = fixture.Join("Viewer"); fixture.Tick();
        std::string cursor;
        for (const auto& message : viewer->messages)
            if (message.Type() == "DirectoryPage") cursor = String(*message.Body(), "cursor");
        fixture.Ack(viewer, cursor);
        JsonValue::Object ack; ack.emplace("cursor", JsonValue(cursor));
        Check(!fixture.Dispatch(viewer, "DirectoryAck", JsonValue(std::move(ack))).IsOk() &&
            viewer->state == SessionState::Closed, "duplicate ACK was accepted after clearing the window");
    }
    {
        Fixture fixture; fixture.Join("Existing"); auto viewer = fixture.Join("Viewer");
        Check(fixture.State(viewer, 0, 0).IsOk(), "timeout fixture position failed"); fixture.Tick();
        fixture.backend->Tick(fixture.now + SummitServerBackend::SynchronizationTimeoutMilliseconds);
        Check(viewer->state == SessionState::Closed && viewer->error == ErrorCode::Timeout,
            "unacknowledged directory page had no absolute timeout");
    }
    {
        Fixture fixture; auto player = fixture.Join("NoPosition"); fixture.Sync();
        fixture.backend->Tick(fixture.now + SummitServerBackend::SynchronizationTimeoutMilliseconds);
        Check(player->state == SessionState::Closed, "joined player without initial state lived indefinitely");
    }
    for (double coordinate : {-1'000'000.0, 1'000'000.0})
    {
        Fixture fixture; auto player = fixture.Join("Boundary");
        Check(fixture.State(player, coordinate, coordinate, 0, true).IsOk(), "inclusive world boundary rejected");
        fixture.Sync(); Check(player->state == SessionState::Authenticated, "boundary grid conversion failed");
        Check(!fixture.State(player, coordinate * 1.00001, 0).IsOk() && player->state == SessionState::Closed,
            "out-of-range coordinate reached floor/int conversion");
        Check(fixture.backend->SnapshotAoiMetrics().positionedPlayers == 0, "closed boundary state remained cached");
    }
    {
        Fixture fixture; fixture.Join("Existing"); auto viewer = fixture.Join("FailedPage");
        viewer->failType = "DirectoryPage"; fixture.Tick();
        Check(viewer->state == SessionState::Authenticated && viewer->Count("DirectoryPage") == 0 &&
            fixture.backend->SnapshotAoiMetrics().joinedPlayers == 2,
            "recoverable directory backpressure disconnected or advanced an unsent page");
        fixture.Sync();
        Check(viewer->Items("DirectoryPage", "players").size() == 1 && viewer->Count("DirectoryReady") == 1,
            "directory retry skipped an identity, duplicated a page, or never completed");
    }
    {
        Fixture fixture; auto viewer = fixture.Join("FailedEnter"); auto other = fixture.Join("Other");
        Check(fixture.State(viewer, 0, 0).IsOk() && fixture.State(other, 1, 1).IsOk(),
            "failed visibility fixture position was invalid");
        viewer->failType = "VisibilityEnter"; fixture.Tick();
        const auto metrics = fixture.backend->SnapshotAoiMetrics();
        Check(viewer->state == SessionState::Authenticated && metrics.joinedPlayers == 2 &&
            viewer->Count("VisibilityEnter") == 0 && viewer->Count("VisibilityReady") == 0,
            "recoverable enter backpressure advanced an unsent relation or disconnected the receiver");
        fixture.Sync();
        Check(viewer->Items("VisibilityEnter", "players").size() == 1 && viewer->Count("VisibilityReady") == 1 &&
            fixture.backend->SnapshotAoiMetrics().visibleEdges == 2,
            "enter retry failed to commit exactly the two successfully delivered relations");
    }
}
}

int main()
{
    try
    {
        TestDirectoryWindowAndLiveProfileChanges();
        TestSpatialBoundariesCoalescingAndReentry();
        TestStationaryMovementAndProfileReplication();
        TestMovingViewerWithinCellAndTeleportNeighborhoods();
        TestBatchBoundsAndStateBackpressure();
        TestSynchronizationErrorsAndCoordinateLimits();
        TestSchedulerWireBudgetsAndLatestState();
        TestSchedulerAgeFairness();
        TestSchedulerGlobalRoundRobin();
        TestSchedulerQueueBacklog();
        TestSchedulerDeferredVisibilityLifetime();
        TestSchedulerReceiverClosesDuringSend();
        std::cout << "Summit AOI tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Summit AOI test failed: " << error.what() << '\n';
        return 1;
    }
}
