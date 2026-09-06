#include "SummitServerBackend.h"

#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Core/Clock.h"
#include "ServerCore/Dispatch/Dispatcher.h"
#include "ServerCore/Protocol/Json.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Session/Session.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCore::Dispatch::Dispatcher;
using ServerCore::Protocol::JsonValue;
using ServerCore::Protocol::Message;
using ServerCore::Protocol::MessageFields;
using ServerCore::Session::Session;
using ServerCore::Session::SessionId;
using ServerCore::Session::SessionState;

class RecordingLogger final : public ServerCore::Core::ILogger
{
public:
    void Write(const ServerCore::Core::LogLevel level, const std::string_view message) noexcept override
    {
        if (discard || level != ServerCore::Core::LogLevel::Info) return;
        try { lines.emplace_back(message); }
        catch (...) {}
    }

    std::vector<std::string> lines;
    bool discard = false;
};

struct ScopedLogCapture
{
    std::shared_ptr<RecordingLogger> logger = std::make_shared<RecordingLogger>();

    ScopedLogCapture() { ServerCore::Core::SetGlobalLogger(logger); }
    ~ScopedLogCapture() { ServerCore::Core::SetGlobalLogger(nullptr); }
};

class FakeSession final : public Session
{
public:
    explicit FakeSession(const std::uint64_t id)
        : mId(static_cast<SessionId>(id))
    {
    }

    [[nodiscard]] SessionId Id() const noexcept override
    {
        return mId;
    }

    [[nodiscard]] SessionState State() const noexcept override
    {
        return mState;
    }

    [[nodiscard]] Status MarkAuthenticated() override
    {
        if (mState == SessionState::Authenticated)
        {
            return Status::Fail(ErrorCode::AlreadyExists, "already authenticated");
        }
        if (mState != SessionState::Connected)
        {
            return Status::Fail(ErrorCode::Closed, "session is closed");
        }
        mState = SessionState::Authenticated;
        return Status::Ok();
    }

    [[nodiscard]] Status Send(const MessageFields& fields) override
    {
        if (mState == SessionState::Closing || mState == SessionState::Closed)
        {
            return Status::Fail(ErrorCode::Closed, "session is closed");
        }
        if (mFailNextSend)
        {
            mFailNextSend = false;
            return Status::Fail(ErrorCode::WouldBlock, "injected send backpressure");
        }
        if (mThrowNextSend)
        {
            mThrowNextSend = false;
            throw std::bad_alloc();
        }

        if (std::function<void()> callback = std::move(mBeforeNextSend))
        {
            mBeforeNextSend = {};
            callback();
        }

        ServerCore::Core::Result<std::vector<std::byte>> serialized =
            ServerCore::Protocol::SerializeMessage(fields);
        if (!serialized.IsOk())
        {
            return serialized.GetStatus();
        }
        // FakeSession도 production Host의 outbound 본문 상한을 적용한다. 배열 명단이
        // 커져도 실제 TCP Session에서 거절될 프레임을 테스트만 성공시키지 않는다.
        if (serialized.Value().size() > Summit::SummitServerBackend::MaximumBodyBytes)
        {
            return Status::Fail(ErrorCode::TooLarge, "Summit outbound frame exceeds 8 KiB");
        }
        ServerCore::Core::Result<Message> parsed = ServerCore::Protocol::ParseMessage(
            std::span<const std::byte>(serialized.Value()));
        if (!parsed.IsOk())
        {
            return parsed.GetStatus();
        }
        mDeliveryOrder.emplace_back(parsed.Value().Type());
        if (parsed.Value().Type() == "ServerNotice")
            mNotices.push_back(std::move(parsed.Value()));
        else if (parsed.Value().Type() == "DirectoryPage" || parsed.Value().Type() == "DirectoryReady" ||
            parsed.Value().Type() == "VisibilityReady")
            mSynchronization.push_back(std::move(parsed.Value()));
        else
            mMessages.push_back(std::move(parsed.Value()));
        return Status::Ok();
    }

    [[nodiscard]] Status SendAndDisconnect(
        const MessageFields& fields, Status reason) override
    {
        const Status sent = Send(fields);
        if (!sent.IsOk())
        {
            return sent;
        }
        mDisconnectReason = std::move(reason);
        mState = SessionState::Closing;
        return Status::Ok();
    }

    void Disconnect(Status reason) override
    {
        mDisconnectReason = std::move(reason);
        mState = SessionState::Closed;
        if (std::function<void()> callback = std::move(mOnDisconnect))
        {
            mOnDisconnect = {};
            callback();
        }
    }

    [[nodiscard]] const std::vector<Message>& Messages() const noexcept
    {
        // 기존 명단/채팅 검사는 부가 알림을 제외한 메시지를 검사한다. 알림 본문과 모든
        // 타입의 실제 상대 순서는 아래 전용 회귀에서 Notices/DeliveryOrder로 검사한다.
        return mMessages;
    }

    [[nodiscard]] const std::vector<Message>& Notices() const noexcept { return mNotices; }
    [[nodiscard]] const std::vector<std::string>& DeliveryOrder() const noexcept { return mDeliveryOrder; }
    [[nodiscard]] const std::vector<Message>& Synchronization() const noexcept { return mSynchronization; }
    [[nodiscard]] std::size_t& DirectoryReadIndex() noexcept { return mDirectoryReadIndex; }

    void ClearMessages() noexcept
    {
        mMessages.clear();
        mNotices.clear();
        mDeliveryOrder.clear();
        mSynchronization.clear();
        mDirectoryReadIndex = 0;
    }

    void FailNextSend() noexcept
    {
        mFailNextSend = true;
    }

    void ThrowNextSend() noexcept
    {
        mThrowNextSend = true;
    }

    void BeforeNextSend(std::function<void()> callback)
    {
        mBeforeNextSend = std::move(callback);
    }

    void OnDisconnect(std::function<void()> callback)
    {
        mOnDisconnect = std::move(callback);
    }

    [[nodiscard]] const Status& DisconnectReason() const noexcept
    {
        return mDisconnectReason;
    }

private:
    SessionId mId;
    std::atomic<SessionState> mState{ SessionState::Connected };
    Status mDisconnectReason = Status::Ok();
    std::vector<Message> mMessages;
    std::vector<Message> mNotices;
    std::vector<std::string> mDeliveryOrder;
    std::vector<Message> mSynchronization;
    std::size_t mDirectoryReadIndex = 0;
    bool mFailNextSend = false;
    bool mThrowNextSend = false;
    std::function<void()> mBeforeNextSend;
    std::function<void()> mOnDisconnect;
};

struct Fixture
{
    explicit Fixture(
        const std::size_t players = Summit::SummitServerBackend::PlayerCapacity,
        const std::size_t connections = Summit::SummitServerBackend::ConnectionCapacity)
        : backend(std::make_shared<Summit::SummitServerBackend>(players, connections))
    {
        const Status registered = backend->RegisterHandlers(dispatcher);
        if (!registered.IsOk())
        {
            throw std::runtime_error("could not register SummitServer handlers");
        }
        dispatcher.SetUnknownTypePolicy(ServerCore::Dispatch::UnknownTypePolicy::Disconnect);
        dispatcher.Freeze();
    }

    std::shared_ptr<Summit::SummitServerBackend> backend;
    Dispatcher dispatcher;
    std::uint64_t tickMilliseconds = 0;

    void Tick()
    {
        tickMilliseconds = (std::max)(tickMilliseconds + 50,
            ServerCore::Core::MillisecondsSinceProcessStart());
        backend->Tick(tickMilliseconds);
    }
};

void Expect(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

[[nodiscard]] JsonValue JoinBody(std::string name, const double version = 6.0,
    const double character = 0.0)
{
    JsonValue::Object fields;
    fields.emplace("schemaVersion", JsonValue(version));
    fields.emplace("c", JsonValue(character));
    fields.emplace("name", JsonValue(std::move(name)));
    return JsonValue(std::move(fields));
}

[[nodiscard]] JsonValue ProfileBody(std::string name, const double character)
{
    JsonValue::Object fields;
    fields.emplace("name", JsonValue(std::move(name)));
    fields.emplace("c", JsonValue(character));
    return JsonValue(std::move(fields));
}

[[nodiscard]] JsonValue ChatBody(std::string text)
{
    JsonValue::Object fields;
    fields.emplace("text", JsonValue(std::move(text)));
    fields.emplace("id", JsonValue(std::string("spoofed")));
    fields.emplace("name", JsonValue(std::string("SpoofedName")));
    fields.emplace("t", JsonValue(std::string("spoofed")));
    return JsonValue(std::move(fields));
}

[[nodiscard]] JsonValue PlayerStateBody(const double character = 1.0)
{
    JsonValue::Object fields;
    fields.emplace("x", JsonValue(1.5));
    fields.emplace("y", JsonValue(3.0));
    fields.emplace("vx", JsonValue(-0.25));
    fields.emplace("vy", JsonValue(2.0));
    fields.emplace("f", JsonValue(-1.0));
    fields.emplace("s", JsonValue(std::string("jump")));
    fields.emplace("c", JsonValue(character));
    return JsonValue(std::move(fields));
}

[[nodiscard]] Message MakeMessage(const std::string_view type, const JsonValue& body,
    const JsonValue* sequence = nullptr)
{
    ServerCore::Core::Result<std::vector<std::byte>> serialized =
        ServerCore::Protocol::SerializeMessage(MessageFields{ type, &body, sequence, nullptr });
    if (!serialized.IsOk())
    {
        throw std::runtime_error("could not serialize test message");
    }
    ServerCore::Core::Result<Message> parsed = ServerCore::Protocol::ParseMessage(
        std::span<const std::byte>(serialized.Value()));
    if (!parsed.IsOk())
    {
        throw std::runtime_error("could not parse test message");
    }
    return std::move(parsed.Value());
}

void AcknowledgeDirectory(Dispatcher& dispatcher, const std::shared_ptr<FakeSession>& session)
{
    // 명단은 받은 페이지를 적용한 뒤 한 장씩 ACK한다. FakeSession도 wire cursor를 그대로
    // 사용하며 실제 Send의 8 KiB 제한을 유지한다. 아래 명단 검사는 원본 페이지를 검사한다.
    std::size_t pages = 0;
    while (session->State() == SessionState::Authenticated &&
        session->DirectoryReadIndex() < session->Synchronization().size())
    {
        const Message& received = session->Synchronization()[session->DirectoryReadIndex()++];
        if (received.Type() != "DirectoryPage") continue;
        const JsonValue* cursor = received.Body() ? received.Body()->Find("cursor") : nullptr;
        Expect(cursor && cursor->TryString(), "directory page cursor was not a string");
        Expect(++pages <= Summit::SummitServerBackend::MaximumSessionCapacity, "directory ACK did not converge");
        JsonValue::Object ack;
        ack.emplace("cursor", *cursor);
        const Message acknowledged = MakeMessage("DirectoryAck", JsonValue(std::move(ack)));
        Expect(dispatcher.Dispatch(session, acknowledged).IsOk(), "directory page ACK failed");
    }
}

[[nodiscard]] Status Dispatch(Dispatcher& dispatcher,
    const std::shared_ptr<FakeSession>& session,
    const std::string_view type,
    const JsonValue& body)
{
    const Message message = MakeMessage(type, body);
    const Status dispatched = dispatcher.Dispatch(session, message);
    if (dispatched.IsOk()) AcknowledgeDirectory(dispatcher, session);
    return dispatched;
}

[[nodiscard]] const JsonValue& RequireBody(const Message& message)
{
    const JsonValue* const body = message.Body();
    Expect(body != nullptr, "message body is missing");
    return *body;
}

[[nodiscard]] const JsonValue& RequireField(
    const JsonValue& object, const std::string_view key)
{
    const JsonValue* const value = object.Find(key);
    Expect(value != nullptr, "message field is missing");
    return *value;
}

[[nodiscard]] std::string RequireString(
    const JsonValue& object, const std::string_view key)
{
    const std::string* const value = RequireField(object, key).TryString();
    Expect(value != nullptr, "message field is not a string");
    return *value;
}

[[nodiscard]] std::string RequireErrorCode(const Message& message)
{
    const JsonValue* const error = message.Error();
    Expect(error != nullptr, "error envelope is missing");
    return RequireString(*error, "code");
}

void CompleteDirectory(Fixture& fixture, const std::shared_ptr<FakeSession>& session)
{
    for (std::size_t step = 0; step <= Summit::SummitServerBackend::MaximumSessionCapacity; ++step)
    {
        fixture.Tick();
        AcknowledgeDirectory(fixture.dispatcher, session);
        if (std::any_of(session->Synchronization().begin(), session->Synchronization().end(),
                [](const auto& message) { return message.Type() == "DirectoryReady"; })) return;
        Expect(session->State() == SessionState::Authenticated,
            "directory synchronization closed session " + std::to_string(static_cast<std::uint64_t>(session->Id())) +
            ": " + std::string(session->DisconnectReason().Message()));
    }
    Expect(false, "directory synchronization never completed");
}

void Join(Fixture& fixture,
    const std::shared_ptr<FakeSession>& session,
    std::string name, const double character = 0.0)
{
    const Status joined = Dispatch(
        fixture.dispatcher, session, "Join", JoinBody(std::move(name), 6.0, character));
    Expect(joined.IsOk(), "Join dispatch failed");
    if (session->State() == SessionState::Authenticated) CompleteDirectory(fixture, session);
}

[[nodiscard]] std::vector<JsonValue> DirectoryProfiles(const FakeSession& session)
{
    std::vector<JsonValue> result;
    bool ready = false;
    for (const auto& message : session.Synchronization())
    {
        if (message.Type() == "DirectoryReady") { Expect(!ready, "duplicate DirectoryReady"); ready = true; }
        if (message.Type() != "DirectoryPage") continue;
        Expect(!ready, "directory page followed DirectoryReady");
        const auto* profiles = RequireField(RequireBody(message), "players").TryArray();
        Expect(profiles, "directory page has no player array");
        result.insert(result.end(), profiles->begin(), profiles->end());
    }
    Expect(ready, "directory did not reach DirectoryReady");
    const auto& order = session.DeliveryOrder();
    const auto accepted = std::find(order.begin(), order.end(), "JoinAccepted");
    const auto firstPage = std::find_if(order.begin(), order.end(), [](const auto& type) {
        return type == "DirectoryPage" || type == "DirectoryReady";
    });
    Expect(accepted != order.end() && accepted < firstPage, "directory overtook JoinAccepted");
    return result;
}

[[nodiscard]] std::vector<std::string> NoticeOrder(const FakeSession& session)
{
    std::vector<std::string> result;
    for (const auto& type : session.DeliveryOrder())
        if (type != "DirectoryPage" && type != "DirectoryReady" && type != "VisibilityReady") result.push_back(type);
    return result;
}

void PlaceNearby(Fixture& fixture, const std::shared_ptr<FakeSession>& first,
    const std::shared_ptr<FakeSession>& second)
{
    Expect(Dispatch(fixture.dispatcher, first, "PlayerState", PlayerStateBody()).IsOk(), "first position failed");
    Expect(Dispatch(fixture.dispatcher, second, "PlayerState", PlayerStateBody()).IsOk(), "second position failed");
    fixture.Tick();
    first->ClearMessages();
    second->ClearMessages();
}

[[nodiscard]] const JsonValue& OneState(const Message& message)
{
    Expect(message.Type() == "StateBatch", "replication did not use StateBatch");
    const auto* states = RequireField(RequireBody(message), "states").TryArray();
    Expect(states && states->size() == 1, "expected exactly one replicated state");
    return states->front();
}

void ExpectNotice(const Message& message, const std::string_view kind,
    const std::string_view text, const std::string_view id = {}, const std::string_view name = {})
{
    Expect(message.Type() == "ServerNotice" && !message.Sequence() && !message.Error(),
        "notice did not use the server-only envelope");
    const auto& body = RequireBody(message);
    Expect(RequireString(body, "kind") == kind && RequireString(body, "text") == text &&
        RequireField(body, "t").TryUInt64(), "notice lost kind, original text, or server time");
    if (id.empty())
        Expect(!body.Find("id") && !body.Find("name"), "announcement impersonated a player");
    else
        Expect(RequireString(body, "id") == id && RequireString(body, "name") == name,
            "membership notice lost the final approved identity");
}

void TestPlayerSnapshotContainsOnlyCommittedOwnedProfiles()
{
    Fixture fixture;
    const auto pending = std::make_shared<FakeSession>(50);
    fixture.backend->OnSessionOpened(pending);
    const auto empty = fixture.backend->SnapshotPlayers();
    Expect(empty.IsOk() && empty.Value().empty(), "a pre-Join TCP connection appeared in the player snapshot");

    const auto failed = std::make_shared<FakeSession>(51);
    failed->FailNextSend();
    Expect(!Dispatch(fixture.dispatcher, failed, "Join", JoinBody("Uncommitted")).IsOk(),
        "the snapshot rollback fixture did not fail Join acceptance");
    const auto afterFailure = fixture.backend->SnapshotPlayers();
    Expect(afterFailure.IsOk() && afterFailure.Value().empty(), "a rolled-back Join appeared in the snapshot");

    const auto high = std::make_shared<FakeSession>((std::numeric_limits<std::uint64_t>::max)());
    const auto low = std::make_shared<FakeSession>(7);
    Join(fixture, high, "한\"닉\\이름", 4);
    Join(fixture, low, "LowerId", 1);
    high->ClearMessages();
    low->ClearMessages();
    const auto original = fixture.backend->SnapshotPlayers();
    Expect(original.IsOk() && original.Value().size() == 2 &&
        original.Value()[0].id == low->Id() && original.Value()[1].id == high->Id(),
        "the snapshot lost the full uint64 ID or did not order profiles by ID");
    Expect(original.Value()[0].name == "LowerId" && original.Value()[0].character == 1 &&
        original.Value()[1].name == "한\"닉\\이름" && original.Value()[1].character == 4,
        "the snapshot did not preserve approved names and characters");
    Expect(high->Messages().empty() && low->Messages().empty() &&
        high->Notices().empty() && low->Notices().empty(), "listing players sent a network message");

    Expect(Dispatch(fixture.dispatcher, high, "SetProfile", ProfileBody("상여자", 0)).IsOk(),
        "the snapshot profile-change fixture failed");
    Expect(Dispatch(fixture.dispatcher, low, "SetProfile", ProfileBody("상여자", 4)).IsOk(),
        "the duplicate profile rejection failed");
    const auto current = fixture.backend->SnapshotPlayers();
    Expect(current.IsOk() && current.Value().size() == 2 && current.Value()[0].name == "LowerId" &&
        current.Value()[0].character == 1 && current.Value()[1].name == "상여자" && current.Value()[1].character == 5,
        "a new snapshot ignored an approved profile or included a rejected profile");
    Expect(original.Value()[1].name == "한\"닉\\이름" && original.Value()[1].character == 4,
        "a snapshot borrowed mutable backend profile data");
    fixture.backend->OnSessionClosed(low->Id(), Status::Ok());
    fixture.backend->OnSessionClosed(low->Id(), Status::Ok());
    const auto remaining = fixture.backend->SnapshotPlayers();
    Expect(remaining.IsOk() && remaining.Value().size() == 1 && remaining.Value()[0].id == high->Id(),
        "a closed player remained in the current snapshot");
    fixture.backend->OnSessionClosed(high->Id(), Status::Ok());
    const auto final = fixture.backend->SnapshotPlayers();
    Expect(final.IsOk() && final.Value().empty() && current.Value().size() == 2 &&
        current.Value()[1].name == "상여자", "closing players invalidated a previously owned snapshot");
}

void TestMembershipNoticesDescribeActualCommitsOnly()
{
    Fixture fixture;
    const auto first = std::make_shared<FakeSession>(5001);
    const auto second = std::make_shared<FakeSession>(5002);
    Join(fixture, first, "첫\"닉\\이름");
    Expect(first->Notices().size() == 1 && NoticeOrder(*first) ==
        std::vector<std::string>{"JoinAccepted", "ServerNotice"}, "self join notice was missing or premature");
    ExpectNotice(first->Notices()[0], "join", "첫\"닉\\이름님이 입장했습니다.", "5001", "첫\"닉\\이름");
    first->ClearMessages();
    Join(fixture, second, "둘째");
    Expect(first->Notices().size() == 1 && second->Notices().size() == 1,
        "roster replay generated historical join notices");
    Expect(NoticeOrder(*first) == std::vector<std::string>{"PlayerJoined", "ServerNotice"} &&
        NoticeOrder(*second) == std::vector<std::string>{"JoinAccepted", "ServerNotice"},
        "join notice preceded membership delivery");
    ExpectNotice(second->Notices()[0], "join", "둘째님이 입장했습니다.", "5002", "둘째");

    first->ClearMessages();
    second->ClearMessages();
    const auto rejected = std::make_shared<FakeSession>(5003);
    Join(fixture, rejected, "둘째");
    fixture.backend->OnSessionClosed(rejected->Id(), Status::Ok());
    const auto rolledBack = std::make_shared<FakeSession>(5004);
    rolledBack->OnDisconnect([&]() { fixture.backend->OnSessionClosed(rolledBack->Id(), Status::Ok()); });
    rolledBack->FailNextSend();
    Expect(!Dispatch(fixture.dispatcher, rolledBack, "Join", JoinBody("미승인")).IsOk(),
        "injected Join acceptance failure was ignored");
    Expect(first->Notices().empty() && second->Notices().empty() && rejected->Notices().empty() &&
        rolledBack->Notices().empty(), "rejected or rolled-back identity produced join/leave notice");
    Expect(Dispatch(fixture.dispatcher, second, "SetProfile", ProfileBody("상여자", 2)).IsOk(),
        "profile update before leave failed");
    Expect(first->Notices().empty() && second->Notices().empty(), "profile edit pretended to rejoin");
    first->ClearMessages();
    second->ClearMessages();
    fixture.backend->OnSessionClosed(second->Id(), Status::FailWithoutMessage(ErrorCode::Timeout));
    fixture.backend->OnSessionClosed(second->Id(), Status::FailWithoutMessage(ErrorCode::Closed));
    Expect(first->Notices().size() == 1 && second->Notices().empty() &&
        NoticeOrder(*first) == std::vector<std::string>{"PlayerLeft", "ServerNotice"},
        "duplicate close repeated the notice or leave notice preceded removal");
    ExpectNotice(first->Notices()[0], "leave", "상여자님이 퇴장했습니다.", "5002", "상여자");
    const auto late = std::make_shared<FakeSession>(5005);
    Join(fixture, late, "늦게입장");
    Expect(late->Notices().size() == 1, "late join replayed departed players' notices");
    ExpectNotice(late->Notices()[0], "join", "늦게입장님이 입장했습니다.", "5005", "늦게입장");
}

void TestAnnouncementsValidateOperatorTextAndHaveNoHistory()
{
    ScopedLogCapture capture;
    Fixture fixture;
    Expect(fixture.backend->Announce("아직 빈 방").IsOk(), "valid announcement required a joined player");
    const auto first = std::make_shared<FakeSession>(5101);
    const auto second = std::make_shared<FakeSession>(5102);
    const auto pending = std::make_shared<FakeSession>(5103);
    Join(fixture, first, "FirstNotice");
    Join(fixture, second, "SecondNotice");
    fixture.backend->OnSessionOpened(pending);
    Expect(second->Notices().size() == 1, "a late join replayed announcements from an empty room");
    first->ClearMessages();
    second->ClearMessages();
    capture.logger->lines.clear();
    std::vector<std::string> invalid{"", "   ", std::string(513, 'x'),
        std::string("bad\xc0\xaf", 5), std::string("bad\xed\xa0\x80", 6),
        std::string("bad\xf4\x90\x80\x80", 7), std::string("bad\xe3\x81", 5)};
    for (unsigned char control = 0; control < 32; ++control)
        invalid.push_back(std::string("hello") + static_cast<char>(control));
    invalid.push_back(std::string("hello") + '\x7f');
    for (const auto& text : invalid)
    {
        Expect(fixture.backend->Announce(text).Code() == ErrorCode::InvalidArgument,
            "invalid operator text was accepted or reported the wrong error");
        Expect(first->Notices().empty() && second->Notices().empty() && capture.logger->lines.empty() &&
            first->State() == SessionState::Authenticated && second->State() == SessionState::Authenticated,
            "invalid announcement reached players/logs or disconnected a healthy receiver");
    }
    const std::string text = "  공지 \"환영\" \\ 그대로  ";
    Expect(fixture.backend->Announce(text).IsOk(), "valid Unicode announcement failed");
    for (const auto& player : {first, second})
    {
        Expect(player->Notices().size() == 1 && player->Messages().empty(),
            "announcement was not delivered once to every joined player");
        ExpectNotice(player->Notices()[0], "announcement", text);
    }
    Expect(pending->DeliveryOrder().empty(), "unjoined connection received an announcement");
    Expect(capture.logger->lines.size() == 1 && capture.logger->lines[0].find_first_of("\r\n") ==
        std::string::npos, "announcement did not produce exactly one escaped log line");
    const auto logged = JsonValue::Parse(capture.logger->lines[0]);
    Expect(logged.IsOk() && RequireString(logged.Value(), "event") == "announcement" &&
        RequireString(logged.Value(), "text") == text, "announcement log changed the original text");
    std::string maximum;
    for (int index = 0; index < 170; ++index) maximum += "한";
    maximum += "ab";
    Expect(maximum.size() == 512 && fixture.backend->Announce(maximum).IsOk(),
        "exactly 512 UTF-8 bytes failed or announcements inherited player chat cooldown");
    ExpectNotice(first->Notices().back(), "announcement", maximum);
    Join(fixture, pending, "PendingNowJoined");
    Expect(pending->Notices().size() == 1, "new join replayed earlier announcements");
    ExpectNotice(pending->Notices()[0], "join", "PendingNowJoined님이 입장했습니다.", "5103", "PendingNowJoined");
    // No incoming handler, including for an already authenticated client, grants operator authority.
    for (const std::string_view type : {"ServerNotice", "Announce"})
    {
        const auto attacker = std::make_shared<FakeSession>(type == "ServerNotice" ? 5110 : 5111);
        Join(fixture, attacker, std::string(type));
        first->ClearMessages();
        Expect(Dispatch(fixture.dispatcher, attacker, type, ChatBody("spoofed announcement")).Code() ==
            ErrorCode::UnknownType && attacker->State() == SessionState::Closed && first->Notices().empty(),
            "client request acquired server announcement privileges");
        fixture.backend->OnSessionClosed(attacker->Id(), Status::Ok());
    }
}

void TestNoticeSendFailuresAndReentrantClosePreserveEventOrder()
{
    for (const bool throws : {false, true})
    {
        Fixture fixture;
        const auto first = std::make_shared<FakeSession>(5201);
        const auto failed = std::make_shared<FakeSession>(5202);
        const auto last = std::make_shared<FakeSession>(5203);
        Join(fixture, first, "First");
        Join(fixture, failed, "Failed");
        Join(fixture, last, "Last");
        for (const auto& player : {first, failed, last}) player->ClearMessages();
        failed->OnDisconnect([&]() {
            fixture.backend->OnSessionClosed(failed->Id(), Status::FailWithoutMessage(ErrorCode::Closed));
            fixture.backend->OnSessionClosed(failed->Id(), Status::FailWithoutMessage(ErrorCode::Closed));
        });
        if (throws) failed->ThrowNextSend();
        else failed->FailNextSend();
        Expect(fixture.backend->Announce("계속 전달").IsOk() && failed->State() == SessionState::Closed,
            "one notice send failure aborted the announcement or retained the failed receiver");
        for (const auto& recipient : {first, last})
        {
            Expect(NoticeOrder(*recipient) ==
                std::vector<std::string>{"ServerNotice", "PlayerLeft", "ServerNotice"} &&
                recipient->Notices().size() == 2, "deferred close interrupted announcement order");
            ExpectNotice(recipient->Notices()[0], "announcement", "계속 전달");
            ExpectNotice(recipient->Notices()[1], "leave", "Failed님이 퇴장했습니다.", "5202", "Failed");
        }
    }
    Fixture fixture;
    const auto observer = std::make_shared<FakeSession>(5301);
    const auto newcomer = std::make_shared<FakeSession>(5302);
    Join(fixture, observer, "Observer");
    observer->ClearMessages();
    observer->BeforeNextSend([&]() {
        fixture.backend->OnSessionClosed(newcomer->Id(), Status::Ok());
        fixture.backend->OnSessionClosed(newcomer->Id(), Status::Ok());
    });
    Expect(Dispatch(fixture.dispatcher, newcomer, "Join", JoinBody("BriefJoin")).IsOk(),
        "brief Join failed before its reentrant close");
    Expect(NoticeOrder(*observer) ==
        std::vector<std::string>{"PlayerJoined", "ServerNotice", "PlayerLeft", "ServerNotice"} &&
        observer->Notices().size() == 2, "reentrant close reordered or duplicated committed join/leave notices");
    ExpectNotice(observer->Notices()[0], "join", "BriefJoin님이 입장했습니다.", "5302", "BriefJoin");
    ExpectNotice(observer->Notices()[1], "leave", "BriefJoin님이 퇴장했습니다.", "5302", "BriefJoin");
}

void TestDuplicateNameCanRetryOnSameConnection()
{
    Fixture fixture;
    const auto alice = std::make_shared<FakeSession>(1);
    const auto retrying = std::make_shared<FakeSession>(2);

    Join(fixture, alice, "Alice");
    Expect(alice->State() == SessionState::Authenticated, "first player was not authenticated");
    Expect(alice->Messages().size() == 1, "first player received an unexpected message count");
    Expect(alice->Messages()[0].Type() == "JoinAccepted", "first response was not JoinAccepted");
    Expect(RequireString(RequireBody(alice->Messages()[0]), "id") == "1",
        "JoinAccepted did not preserve the uint64 id as a string");

    const Status duplicate = Dispatch(
        fixture.dispatcher, retrying, "Join", JoinBody("Alice"));
    Expect(duplicate.IsOk(), "duplicate rejection could not be sent");
    Expect(retrying->State() == SessionState::Connected,
        "duplicate name closed the retryable connection");
    Expect(retrying->Messages().size() == 1, "duplicate name did not produce one response");
    Expect(retrying->Messages()[0].Type() == "JoinRejected",
        "duplicate name did not produce JoinRejected");
    Expect(RequireErrorCode(retrying->Messages()[0]) == "name_duplicate",
        "duplicate name used the wrong error code");

    Join(fixture, retrying, "Bob");
    Expect(retrying->State() == SessionState::Authenticated,
        "same-connection nickname retry did not authenticate");
    Expect(retrying->Messages().size() == 2,
        "retrying player did not receive rejection followed by acceptance");
    Expect(retrying->Messages()[1].Type() == "JoinAccepted",
        "nickname retry was not accepted before player events");
    const auto roster = DirectoryProfiles(*retrying);
    Expect(roster.size() == 1 && RequireString(roster.front(), "name") == "Alice",
        "existing profile was missing from the acknowledged directory");
    Expect(alice->Messages().size() == 2 && alice->Messages()[1].Type() == "PlayerJoined",
        "existing player was not told about the new player");
}

void TestPlayerStateIsCoalescedWithoutEchoAndSeedsNewVisibility()
{
    Fixture fixture;
    const auto first = std::make_shared<FakeSession>(11);
    const auto second = std::make_shared<FakeSession>(12);
    Join(fixture, first, "First");
    Join(fixture, second, "Second");
    PlaceNearby(fixture, first, second);

    const Status relayed = Dispatch(
        fixture.dispatcher, first, "PlayerState", PlayerStateBody());
    Expect(relayed.IsOk(), "valid PlayerState dispatch failed");
    Expect(first->Messages().empty(), "PlayerState was echoed to its sender");
    Expect(second->Messages().empty(), "PlayerState bypassed the replication tick");
    fixture.Tick();
    Expect(first->Messages().empty(), "replication tick echoed state to its sender");
    Expect(second->Messages().size() == 1, "PlayerState was not relayed exactly once");
    const Message& state = second->Messages()[0];
    const JsonValue& stateBody = OneState(state);
    Expect(RequireString(stateBody, "id") == "11", "relay used the wrong session id");
    Expect(RequireField(stateBody, "t").TryUInt64() != nullptr,
        "relay did not add a monotonic uint64 timestamp");
    Expect(RequireField(stateBody, "x").TryNumber() != nullptr,
        "relay did not preserve x");
    Expect(RequireField(stateBody, "c").TryUInt64() != nullptr,
        "relay did not normalize the character index to an unsigned integer");
    Expect(*RequireField(stateBody, "c").TryUInt64() == 0,
        "state packet changed the approved character");

    first->ClearMessages();
    second->ClearMessages();
    const auto third = std::make_shared<FakeSession>(13);
    Join(fixture, third, "Third");
    Expect(!third->Messages().empty() && third->Messages()[0].Type() == "JoinAccepted",
        "new player did not receive JoinAccepted first");
    for (const Message& message : third->Messages())
    {
        Expect(message.Type() != "StateBatch" && message.Type() != "VisibilityEnter",
            "unpositioned newcomer received arbitrary world state");
    }
    third->ClearMessages();
    Expect(Dispatch(fixture.dispatcher, third, "PlayerState", PlayerStateBody()).IsOk(), "newcomer position failed");
    fixture.Tick();
    std::size_t entered = 0;
    for (const auto& message : third->Messages())
        if (message.Type() == "VisibilityEnter")
        {
            const auto* players = RequireField(RequireBody(message), "players").TryArray();
            Expect(players, "visibility enter is missing its snapshot array");
            entered += players->size();
        }
    Expect(entered == 2, "positioned newcomer did not receive the two cached nearby players");
}

void TestLeaveReleasesNickname()
{
    Fixture fixture;
    const auto first = std::make_shared<FakeSession>(21);
    const auto observer = std::make_shared<FakeSession>(22);
    Join(fixture, first, "Reusable");
    Join(fixture, observer, "Observer");
    observer->ClearMessages();

    first->Disconnect(Status::Fail(ErrorCode::Closed, "peer closed"));
    fixture.backend->OnSessionClosed(
        first->Id(), Status::Fail(ErrorCode::Closed, "peer closed"));
    Expect(observer->Messages().size() == 1,
        "remaining player did not receive exactly one leave event");
    Expect(observer->Messages()[0].Type() == "PlayerLeft", "leave event used the wrong type");
    Expect(RequireString(RequireBody(observer->Messages()[0]), "id") == "21",
        "leave event used the wrong id");

    const auto replacement = std::make_shared<FakeSession>(23);
    Join(fixture, replacement, "Reusable");
    Expect(replacement->State() == SessionState::Authenticated,
        "disconnected player's nickname was not released");
}

void TestInvalidJoinAndPreJoinStatePolicies()
{
    Fixture fixture;
    const auto invalidName = std::make_shared<FakeSession>(31);
    const Status rejected = Dispatch(
        fixture.dispatcher, invalidName, "Join", JoinBody(""));
    Expect(rejected.IsOk(), "invalid-name rejection could not be sent");
    Expect(invalidName->State() == SessionState::Connected,
        "invalid name closed the retryable connection");
    Expect(RequireErrorCode(invalidName->Messages()[0]) == "name_invalid",
        "invalid name used the wrong error code");

    invalidName->ClearMessages();
    const Status oversized = Dispatch(fixture.dispatcher, invalidName, "Join",
        JoinBody(std::string(Summit::SummitServerBackend::MaximumNameBytes + 1, 'x')));
    Expect(oversized.IsOk(), "oversized-name rejection could not be sent");
    Expect(invalidName->State() == SessionState::Connected,
        "oversized name closed the retryable connection");
    Expect(RequireErrorCode(invalidName->Messages()[0]) == "name_invalid",
        "oversized name used the wrong error code");

    const auto wrongVersion = std::make_shared<FakeSession>(32);
    const Status schema = Dispatch(
        fixture.dispatcher, wrongVersion, "Join", JoinBody("Version", 5));
    Expect(schema.IsOk(), "schema rejection final frame could not be sent");
    Expect(wrongVersion->State() == SessionState::Closing,
        "schema mismatch did not close the connection");
    Expect(RequireErrorCode(wrongVersion->Messages()[0]) == "schema_mismatch",
        "schema mismatch used the wrong error code");

    const auto preJoinState = std::make_shared<FakeSession>(33);
    const Status protocol = Dispatch(
        fixture.dispatcher, preJoinState, "PlayerState", PlayerStateBody());
    Expect(protocol.IsOk(), "protocol rejection final frame could not be sent");
    Expect(preJoinState->State() == SessionState::Closing,
        "pre-Join PlayerState did not close the connection");
    Expect(RequireErrorCode(preJoinState->Messages()[0]) == "protocol_violation",
        "pre-Join PlayerState used the wrong error code");
}

void TestMembershipDeliveryFailureDisconnectsButStateDropDoesNot()
{
    Fixture fixture;
    const auto sender = std::make_shared<FakeSession>(41);
    const auto receiver = std::make_shared<FakeSession>(42);
    Join(fixture, sender, "Sender");
    Join(fixture, receiver, "Receiver");
    PlaceNearby(fixture, sender, receiver);

    receiver->FailNextSend();
    const Status state = Dispatch(
        fixture.dispatcher, sender, "PlayerState", PlayerStateBody());
    Expect(state.IsOk(), "state broadcast failed as a whole");
    fixture.Tick();
    Expect(receiver->State() == SessionState::Authenticated,
        "droppable StateBatch backpressure disconnected the receiver");

    receiver->FailNextSend();
    sender->Disconnect(Status::Fail(ErrorCode::Closed, "peer closed"));
    fixture.backend->OnSessionClosed(
        sender->Id(), Status::Fail(ErrorCode::Closed, "peer closed"));
    Expect(receiver->State() == SessionState::Closed,
        "failed reliable PlayerLeft delivery left an inconsistent receiver connected");
}

void TestMaximumSessionIdUsesDecimalString()
{
    Fixture fixture;
    const auto session = std::make_shared<FakeSession>((std::numeric_limits<std::uint64_t>::max)());
    Join(fixture, session, "MaximumId");
    Expect(RequireString(RequireBody(session->Messages()[0]), "id") ==
            "18446744073709551615",
        "maximum uint64 SessionId lost precision on the wire");
}

void TestJoinExceptionRollsBackIdentityWithoutPhantomLeave()
{
    Fixture fixture;
    const auto observer = std::make_shared<FakeSession>(51);
    const auto failing = std::make_shared<FakeSession>(52);
    Join(fixture, observer, "Observer");
    observer->ClearMessages();

    failing->ThrowNextSend();
    const Status failedJoin = Dispatch(
        fixture.dispatcher, failing, "Join", JoinBody("Transient"));
    Expect(failedJoin.Code() == ErrorCode::PlatformError,
        "throwing Join send was not converted to PlatformError");
    Expect(failing->State() == SessionState::Closed,
        "throwing Join send did not disconnect its session");
    Expect(observer->Messages().empty(),
        "failed Join emitted PlayerJoined before commit");

    fixture.backend->OnSessionClosed(
        failing->Id(), Status::Fail(ErrorCode::Closed, "failed join closed"));
    Expect(observer->Messages().empty(),
        "rolled-back Join emitted a phantom PlayerLeft");

    const auto replacement = std::make_shared<FakeSession>(53);
    Join(fixture, replacement, "Transient");
    Expect(replacement->State() == SessionState::Authenticated,
        "rolled-back Join kept its nickname reserved");
}

void TestUnjoinedSessionExpiresOnAbsoluteDeadline()
{
    Fixture fixture;
    const auto pending = std::make_shared<FakeSession>(61);
    fixture.backend->OnSessionOpened(pending);
    fixture.backend->ExpireUnjoinedSessions((std::numeric_limits<std::uint64_t>::max)());
    Expect(pending->State() == SessionState::Closed,
        "unjoined connection survived its absolute Join deadline");
    Expect(pending->DisconnectReason().Code() == ErrorCode::Timeout,
        "expired unjoined connection used the wrong disconnect reason");

    const auto joined = std::make_shared<FakeSession>(62);
    fixture.backend->OnSessionOpened(joined);
    Join(fixture, joined, "JoinedBeforeDeadline");
    fixture.backend->ExpireUnjoinedSessions((std::numeric_limits<std::uint64_t>::max)());
    Expect(joined->State() == SessionState::Authenticated,
        "successful Join remained in the absolute deadline set");
}

void TestProfilesAreAtomicAndAuthoritative()
{
    Fixture fixture;
    const auto alice = std::make_shared<FakeSession>(201);
    const auto bob = std::make_shared<FakeSession>(202);
    Join(fixture, alice, "Alice");
    Join(fixture, bob, "Bob");
    PlaceNearby(fixture, alice, bob);
    alice->ClearMessages();
    bob->ClearMessages();
    const auto profile = [](const std::string& name, const double character)
    {
        JsonValue::Object fields;
        fields.emplace("name", JsonValue(name));
        fields.emplace("c", JsonValue(character));
        return JsonValue(std::move(fields));
    };
    const auto changed = Dispatch(fixture.dispatcher, alice, "SetProfile", profile("NewAlice", 1));
    Expect(changed.IsOk(), "profile update failed");
    for (const auto& session : {alice, bob})
    {
        Expect(session->Messages().size() == 1 && session->Messages()[0].Type() == "ProfileChanged",
            "profile update did not reach sender and observer");
        const auto& body = RequireBody(session->Messages()[0]);
        Expect(RequireString(body, "name") == "NewAlice" && RequireString(body, "id") == "201",
            "profile update had wrong identity");
        Expect(*RequireField(body, "c").TryUInt64() == 1, "profile update had wrong character");
        session->ClearMessages();
    }
    for (const auto& invalid : { profile("Bob", 0), profile("", 0), profile("   ", 0),
            profile("NewAlice", 5), profile("NewAlice", -1), profile("NewAlice", 0.5),
            profile(std::string(49, 'x'), 0), profile("line\nbreak", 0) })
    {
        Expect(Dispatch(fixture.dispatcher, alice, "SetProfile", invalid).IsOk(), "rejection failed");
        Expect(alice->Messages().back().Type() == "ProfileRejected", "invalid profile was accepted");
        Expect(alice->State() == SessionState::Authenticated, "retryable profile error disconnected");
        Expect(bob->Messages().empty(), "rejected profile was broadcast");
    }
    alice->ClearMessages();
    Expect(Dispatch(fixture.dispatcher, alice, "PlayerState", PlayerStateBody(0)).IsOk(), "state failed");
    fixture.Tick();
    Expect(*RequireField(OneState(bob->Messages().back()), "c").TryUInt64() == 1,
        "rejected edit or stale state changed approved character");
    bob->ClearMessages();

    const auto newcomer = std::make_shared<FakeSession>(203);
    Join(fixture, newcomer, "Alice");
    Expect(newcomer->State() == SessionState::Authenticated, "previous nickname not released");
    const auto roster = DirectoryProfiles(*newcomer);
    Expect(roster.size() == 2, "newcomer did not receive full directory");
    bool found = false;
    for (const auto& entry : roster)
        if (RequireString(entry, "id") == "201")
        {
            found = true;
            Expect(RequireString(entry, "name") == "NewAlice" &&
                *RequireField(entry, "c").TryUInt64() == 1, "roster lost approved profile");
        }
    Expect(found, "updated player missing from roster");
    alice->ClearMessages();
    bob->ClearMessages();
    Expect(Dispatch(fixture.dispatcher, alice, "SetProfile", profile("NewAlice", 0)).IsOk(),
        "character-only update failed");
    Expect(alice->Messages().back().Type() == "ProfileChanged", "own name rejected as duplicate");

    bob->FailNextSend();
    Expect(Dispatch(fixture.dispatcher, alice, "SetProfile", profile("NewAlice", 1)).IsOk(),
        "reliable broadcast failed as a whole");
    Expect(bob->State() == SessionState::Closed, "profile delivery failure kept stale receiver");

    fixture.backend->OnSessionClosed(bob->Id(), Status::Fail(ErrorCode::Closed, "test cleanup"));
    alice->ClearMessages();
    newcomer->ThrowNextSend();
    Expect(Dispatch(fixture.dispatcher, alice, "SetProfile", profile("NewAlice", 0)).IsOk(),
        "one throwing receiver aborted profile broadcast");
    Expect(newcomer->State() == SessionState::Closed, "throwing receiver remained connected");
    Expect(alice->Messages().back().Type() == "ProfileChanged",
        "throwing receiver prevented sender acknowledgement");

    const auto unjoined = std::make_shared<FakeSession>(204);
    Expect(Dispatch(fixture.dispatcher, unjoined, "SetProfile", profile("Early", 0)).IsOk(),
        "pre-join rejection failed");
    Expect(unjoined->State() == SessionState::Closing, "pre-join profile was allowed");
}
void ExpectProfile(const JsonValue& body, const std::string_view name,
    const std::uint64_t character)
{
    const auto* approvedCharacter = RequireField(body, "c").TryUInt64();
    Expect(RequireString(body, "name") == name && approvedCharacter &&
        *approvedCharacter == character, "profile did not carry the server-approved character");
}

void ExpectJoinedProfile(const FakeSession& newcomer, const std::string_view existingName,
    const std::uint64_t existingCharacter)
{
    Expect(newcomer.Messages().size() == 1 && newcomer.Messages()[0].Type() == "JoinAccepted",
        "newcomer membership ordering changed");
    const auto* inlineRoster = RequireField(RequireBody(newcomer.Messages()[0]), "players").TryArray();
    Expect(inlineRoster && inlineRoster->empty(), "schema 6 JoinAccepted unexpectedly duplicated the directory");
    const auto roster = DirectoryProfiles(newcomer);
    Expect(roster.size() == 1, "newcomer did not receive the existing profile");
    ExpectProfile(roster.front(), existingName, existingCharacter);
}

void TestAllOrdinaryCharactersCanJoinAndChangeProfile()
{
    for (std::uint64_t character = 0; character <= 4; ++character)
    {
        Fixture fixture;
        const auto player = std::make_shared<FakeSession>(801);
        const auto observer = std::make_shared<FakeSession>(802);
        Join(fixture, player, "Player", static_cast<double>(character));
        Expect(player->State() == SessionState::Authenticated, "ordinary character Join was rejected");
        ExpectProfile(RequireBody(player->Messages()[0]), "Player", character);
        Expect(*RequireField(RequireBody(player->Messages()[0]), "schemaVersion").TryUInt64() == 6,
            "JoinAccepted did not negotiate AOI schema v6");
        Join(fixture, observer, "Observer");
        ExpectJoinedProfile(*observer, "Player", character);
        PlaceNearby(fixture, player, observer);
        player->ClearMessages();
        observer->ClearMessages();
        Expect(Dispatch(fixture.dispatcher, player, "SetProfile",
            ProfileBody("Renamed", static_cast<double>(character))).IsOk(), "ordinary profile failed");
        for (const auto& recipient : {player, observer})
        {
            Expect(recipient->Messages().size() == 1 &&
                recipient->Messages()[0].Type() == "ProfileChanged", "profile did not reach everyone");
            ExpectProfile(RequireBody(recipient->Messages()[0]), "Renamed", character);
            recipient->ClearMessages();
        }
        // A stale tdw movement packet after changing the nickname cannot reselect tdw.
        Expect(Dispatch(fixture.dispatcher, player, "PlayerState", PlayerStateBody(5)).IsOk(),
            "catalog character in movement packet was rejected");
        fixture.Tick();
        Expect(*RequireField(OneState(observer->Messages().back()), "c").TryUInt64() == character,
            "movement packet bypassed the approved ordinary character");
    }
}

void TestChatIsAuthoritativeEchoedAndRateLimitedWithoutHistory()
{
    Fixture fixture;
    const auto first = std::make_shared<FakeSession>(901);
    const auto second = std::make_shared<FakeSession>(902);
    Join(fixture, first, "FirstChat");
    Join(fixture, second, "SecondChat");
    first->ClearMessages();
    second->ClearMessages();
    const std::string originalText = "  안녕 👋  ";
    Expect(Dispatch(fixture.dispatcher, first, "Chat", ChatBody(originalText)).IsOk(),
        "the first chat was not immediately accepted");
    for (const auto& recipient : {first, second})
    {
        Expect(recipient->Messages().size() == 1 &&
            recipient->Messages()[0].Type() == "ChatMessage", "chat was not echoed to every player");
        const auto& body = RequireBody(recipient->Messages()[0]);
        Expect(RequireString(body, "id") == "901" && RequireString(body, "name") == "FirstChat" &&
            RequireString(body, "text") == originalText && RequireField(body, "t").TryUInt64(),
            "chat changed Unicode/spaces or trusted client identity/name/time");
    }
    const Message oldChat = first->Messages()[0];
    const std::uint64_t firstTime = *RequireField(RequireBody(oldChat), "t").TryUInt64();
    Expect(Dispatch(fixture.dispatcher, first, "Chat", ChatBody("too soon")).IsOk(),
        "chat rate rejection did not produce an ordinary response");
    Expect(first->Messages().back().Type() == "ChatRejected" &&
        RequireErrorCode(first->Messages().back()) == "chat_rate_limited" &&
        second->Messages().size() == 1 && first->State() == SessionState::Authenticated,
        "chat burst was broadcast or closed its sender");

    Expect(Dispatch(fixture.dispatcher, second, "Chat", ChatBody("!")).IsOk(),
        "one player's chat cooldown blocked another player's first message");
    for (const auto& recipient : {first, second})
    {
        Expect(recipient->Messages().back().Type() == "ChatMessage" &&
            RequireString(RequireBody(recipient->Messages().back()), "id") == "902",
            "independent chat was not broadcast including its sender");
    }
    Expect(Dispatch(fixture.dispatcher, first, "SetProfile", ProfileBody("RenamedChat", 2)).IsOk(),
        "profile change during chat cooldown failed");
    Expect(Dispatch(fixture.dispatcher, first, "Chat", ChatBody("still too soon")).IsOk() &&
        RequireErrorCode(first->Messages().back()) == "chat_rate_limited",
        "changing profile bypassed the per-session chat cooldown");
    Expect(RequireString(RequireBody(oldChat), "name") == "FirstChat",
        "a profile change rewrote an earlier chat's nickname snapshot");

    std::this_thread::sleep_for(std::chrono::milliseconds(
        Summit::SummitServerBackend::ChatCooldownMilliseconds + 10));
    first->ClearMessages();
    second->ClearMessages();
    Expect(Dispatch(fixture.dispatcher, first, "Chat", ChatBody("after cooldown")).IsOk(),
        "a valid chat did not recover after the cooldown");
    for (const auto& recipient : {first, second})
    {
        Expect(recipient->Messages().size() == 1 &&
            recipient->Messages()[0].Type() == "ChatMessage", "recovered chat was not broadcast");
        const auto& body = RequireBody(recipient->Messages()[0]);
        Expect(RequireString(body, "name") == "RenamedChat" &&
            *RequireField(body, "t").TryUInt64() >= firstTime +
                Summit::SummitServerBackend::ChatCooldownMilliseconds,
            "chat after profile change retained the wrong name or timestamp");
    }
    const auto late = std::make_shared<FakeSession>(903);
    Join(fixture, late, "LateChat");
    Expect(late->Messages().size() == 1 && late->Messages()[0].Type() == "JoinAccepted" &&
        DirectoryProfiles(*late).size() == 2, "late join replayed earlier chat messages or lost its directory");
    first->ClearMessages();
    second->ClearMessages();
    late->ClearMessages();
    Expect(Dispatch(fixture.dispatcher, late, "Chat", ChatBody("new arrival")).IsOk(),
        "a late joiner could not send its first chat");
    for (const auto& recipient : {first, second, late})
    {
        Expect(recipient->Messages().size() == 1 &&
            RequireString(RequireBody(recipient->Messages()[0]), "name") == "LateChat",
            "a current chat did not reach all three joined players");
    }
}

void TestChatValidationRetainsConnectionAndDoesNotConsumeCooldown()
{
    Fixture fixture;
    const auto first = std::make_shared<FakeSession>(911);
    const auto observer = std::make_shared<FakeSession>(912);
    Join(fixture, first, "ChatValidation");
    Join(fixture, observer, "Observer");
    observer->ClearMessages();
    std::vector<JsonValue> invalidBodies{
        ChatBody(""), ChatBody("   "),
        ChatBody(std::string(Summit::SummitServerBackend::MaximumChatBytes + 1, 'a')),
        JsonValue(JsonValue::Object{}) };
    for (const JsonValue& invalidText : {JsonValue(JsonValue::Object{}),
             JsonValue(JsonValue::Array{}), JsonValue(nullptr), JsonValue(123), JsonValue(false)})
    {
        JsonValue::Object wrongType;
        wrongType.emplace("text", invalidText);
        invalidBodies.emplace_back(std::move(wrongType));
    }
    std::string oversizedUnicode;
    for (int index = 0; index < 171; ++index) oversizedUnicode += "한";
    invalidBodies.push_back(ChatBody(oversizedUnicode));
    for (unsigned char control = 0; control < 32; ++control)
    {
        invalidBodies.push_back(ChatBody(std::string("hello") + static_cast<char>(control)));
    }
    invalidBodies.push_back(ChatBody(std::string("hello") + '\x7f'));
    const JsonValue sequence((std::numeric_limits<std::uint64_t>::max)());
    for (const auto& invalid : invalidBodies)
    {
        first->ClearMessages();
        const Message message = MakeMessage("Chat", invalid, &sequence);
        Expect(fixture.dispatcher.Dispatch(first, message).IsOk(), "invalid chat response failed");
        Expect(first->State() == SessionState::Authenticated && first->Messages().size() == 1 &&
            first->Messages()[0].Type() == "ChatRejected" &&
            RequireErrorCode(first->Messages()[0]) == "chat_invalid" && observer->Messages().empty(),
            "invalid text was accepted or closed the connection");
        const auto* echoedSequence = first->Messages()[0].Sequence();
        Expect(echoedSequence && echoedSequence->TryUInt64() &&
            *echoedSequence->TryUInt64() == (std::numeric_limits<std::uint64_t>::max)(),
            "chat rejection did not preserve the caller's exact sequence");
    }
    std::string maximumUnicode;
    for (int index = 0; index < 170; ++index) maximumUnicode += "한";
    maximumUnicode += "ab";
    Expect(maximumUnicode.size() == Summit::SummitServerBackend::MaximumChatBytes,
        "the Unicode chat fixture must measure exactly 512 UTF-8 bytes");
    first->ClearMessages();
    Expect(Dispatch(fixture.dispatcher, first, "Chat", ChatBody(maximumUnicode)).IsOk() &&
        first->Messages().size() == 1 && first->Messages()[0].Type() == "ChatMessage" &&
        RequireString(RequireBody(first->Messages()[0]), "text") == maximumUnicode &&
        observer->Messages().size() == 1,
        "invalid requests armed the cooldown or the exact byte limit was rejected");

    Fixture preJoinFixture;
    const auto unjoined = std::make_shared<FakeSession>(913);
    Expect(Dispatch(preJoinFixture.dispatcher, unjoined, "Chat", ChatBody("early")).IsOk() &&
        unjoined->State() == SessionState::Closing &&
        RequireErrorCode(unjoined->Messages()[0]) == "protocol_violation",
        "pre-Join chat was allowed instead of receiving terminal join rejection");

    // Invalid UTF-8 never reaches game handlers: the shared JSON envelope parser rejects it.
    for (const std::string bytes : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
             std::string("\xf4\x90\x80\x80", 4), std::string("\x80", 1)})
    {
        const std::string raw = "{\"type\":\"Chat\",\"body\":{\"text\":\"" + bytes + "\"}}";
        const auto parsed = ServerCore::Protocol::ParseMessage(
            std::as_bytes(std::span(raw.data(), raw.size())));
        Expect(!parsed.IsOk() && parsed.GetStatus().Code() == ErrorCode::InvalidFormat,
            "malformed UTF-8 crossed the JSON boundary into a chat message");
    }
}

void TestChatSendFailuresPreserveOtherRecipientsAndMembershipOrder()
{
    for (const bool throwSend : {false, true})
    {
        Fixture fixture;
        const auto sender = std::make_shared<FakeSession>(921);
        const auto failed = std::make_shared<FakeSession>(922);
        const auto observer = std::make_shared<FakeSession>(923);
        Join(fixture, sender, "Sender");
        Join(fixture, failed, "Failed");
        Join(fixture, observer, "Observer");
        for (const auto& recipient : {sender, failed, observer}) recipient->ClearMessages();
        failed->OnDisconnect([&]
        {
            fixture.backend->OnSessionClosed(failed->Id(), Status::FailWithoutMessage(ErrorCode::Closed));
        });
        if (throwSend) failed->ThrowNextSend();
        else failed->FailNextSend();
        Expect(Dispatch(fixture.dispatcher, sender, "Chat", ChatBody("still delivered")).IsOk(),
            "one failing chat recipient aborted broadcast");
        Expect(failed->State() == SessionState::Closed && failed->Messages().empty(),
            "a receiver that missed a chat stayed connected with inconsistent history");
        for (const auto& recipient : {sender, observer})
        {
            Expect(recipient->Messages().size() == 2 &&
                recipient->Messages()[0].Type() == "ChatMessage" &&
                recipient->Messages()[1].Type() == "PlayerLeft" &&
                RequireString(RequireBody(recipient->Messages()[1]), "id") == "922",
                "reentrant close interrupted chat delivery or reordered membership events");
        }
    }
}

void TestNicknameCharacterIsAutomaticAndAuthoritative()
{
    for (const std::string specialName : {std::string("상여자"), std::string("심심이심셔")})
    {
        Fixture fixture;
        const auto player = std::make_shared<FakeSession>(811);
        const auto observer = std::make_shared<FakeSession>(812);
        Join(fixture, observer, "Observer");
        observer->ClearMessages();
        Join(fixture, player, specialName, 4);
        Expect(player->State() == SessionState::Authenticated, "special nickname Join was rejected");
        ExpectProfile(RequireBody(player->Messages()[0]), specialName, 5);
        Expect(observer->Messages().size() == 1 && observer->Messages()[0].Type() == "PlayerJoined",
            "automatic character did not reach the existing client");
        ExpectProfile(RequireBody(observer->Messages()[0]), specialName, 5);
        PlaceNearby(fixture, player, observer);
        player->ClearMessages();
        observer->ClearMessages();

        for (const std::string name : {std::string("Ordinary"), specialName})
        {
            Expect(Dispatch(fixture.dispatcher, player, "SetProfile", ProfileBody(name, 5)).IsOk(),
                "direct tdw profile rejection failed");
            Expect(player->Messages().back().Type() == "ProfileRejected" &&
                RequireErrorCode(player->Messages().back()) == "character_invalid" &&
                player->State() == SessionState::Authenticated, "direct tdw profile was allowed");
            Expect(observer->Messages().empty(), "rejected tdw request was broadcast");
        }
        const auto retrying = std::make_shared<FakeSession>(813);
        for (const std::string name : {std::string("Ordinary"), specialName})
        {
            Expect(Dispatch(fixture.dispatcher, retrying, "Join", JoinBody(name, 6, 5)).IsOk(),
                "direct tdw Join rejection failed");
            Expect(retrying->Messages().back().Type() == "JoinRejected" &&
                RequireErrorCode(retrying->Messages().back()) == "character_invalid" &&
                retrying->State() == SessionState::Connected, "direct tdw Join was allowed");
        }
        player->ClearMessages();
        Expect(Dispatch(fixture.dispatcher, player, "PlayerState", PlayerStateBody(5)).IsOk(),
            "approved tdw could not relay movement");
        fixture.Tick();
        Expect(*RequireField(OneState(observer->Messages().back()), "c").TryUInt64() == 5,
            "failed profile edit changed the approved tdw character");
        observer->ClearMessages();

        for (const auto& change : {std::pair{ " " + specialName, 2u },
                 std::pair{ specialName + " ", 3u }, std::pair{ specialName, 5u },
                 std::pair{ std::string("Ordinary"), 4u }})
        {
            const double requested = change.second == 5 ? 0.0 : static_cast<double>(change.second);
            Expect(Dispatch(fixture.dispatcher, player, "SetProfile",
                ProfileBody(change.first, requested)).IsOk(), "nickname transition failed");
            for (const auto& recipient : {player, observer})
            {
                Expect(recipient->Messages().size() == 1 &&
                    recipient->Messages()[0].Type() == "ProfileChanged",
                    "nickname transition did not reach everyone");
                ExpectProfile(RequireBody(recipient->Messages()[0]), change.first, change.second);
                recipient->ClearMessages();
            }
        }
        // A subsequent ordinary choice must not stick while the special name remains approved.
        Expect(Dispatch(fixture.dispatcher, player, "SetProfile", ProfileBody(specialName, 1)).IsOk(),
            "return to the special nickname failed");
        observer->ClearMessages();
        fixture.backend->OnSessionClosed(observer->Id(), Status::Ok());
        player->ClearMessages();
        retrying->ClearMessages();
        Join(fixture, retrying, "LateObserver", 3);
        ExpectJoinedProfile(*retrying, specialName, 5);
    }
}

void TestCapacityRejectsOnlyTheSeventeenthPlayer()
{
    Fixture fixture;
    std::vector<std::shared_ptr<FakeSession>> joined;
    joined.reserve(Summit::SummitServerBackend::PlayerCapacity);
    for (std::size_t index = 0; index < Summit::SummitServerBackend::PlayerCapacity; ++index)
    {
        auto session = std::make_shared<FakeSession>(100 + index);
        Join(fixture, session, "Player" + std::to_string(index));
        Expect(session->State() == SessionState::Authenticated,
            "one of the first sixteen players was rejected");
        joined.push_back(std::move(session));
    }

    const auto overflow = std::make_shared<FakeSession>(1000);
    const Status full = Dispatch(
        fixture.dispatcher, overflow, "Join", JoinBody("Overflow"));
    Expect(full.IsOk(), "server-full final frame could not be sent");
    Expect(overflow->State() == SessionState::Closing,
        "seventeenth joined player was not disconnected");
    Expect(RequireErrorCode(overflow->Messages()[0]) == "server_full",
        "capacity rejection used the wrong error code");
}

void TestConfiguredCapacityAndLargeRosterStayCompatible()
{
    constexpr std::size_t Capacity = 80;
    Fixture fixture(Capacity, Capacity);
    Expect(fixture.backend->GetPlayerCapacity() == Capacity &&
        fixture.backend->GetConnectionCapacity() == Capacity,
        "configured capacity was not retained by the backend");
    std::vector<std::shared_ptr<FakeSession>> players;
    for (std::size_t index = 0; index < Capacity; ++index)
    {
        const auto player = std::make_shared<FakeSession>(2000 + index);
        // Quotes are valid nickname bytes but require JSON escaping. At this size an
        // unbounded JoinAccepted roster exceeds 8 KiB, even though each profile is valid.
        Join(fixture, player, std::string(40, '"') + std::to_string(index));
        Expect(player->State() == SessionState::Authenticated,
            "a configured-capacity Join failed under the real outbound frame bound");
        const auto& messages = player->Messages();
        Expect(messages.size() == 1 && messages.front().Type() == "JoinAccepted",
            "large Join must accept first without mixing directory entries with live join events");
        const auto& body = RequireBody(messages.front());
        const auto* capacity = RequireField(body, "capacity").TryUInt64();
        const auto* roster = RequireField(body, "players").TryArray();
        Expect(capacity && *capacity == Capacity && roster,
            "JoinAccepted lost its schema-6 capacity or players array");
        Expect(roster->empty(), "schema 6 JoinAccepted unexpectedly duplicated the directory");
        const auto directory = DirectoryProfiles(*player);
        Expect(directory.size() == index, "paged directory omitted an existing player");
        std::vector<bool> received(index, false);
        for (const auto& profile : directory)
        {
            const std::uint64_t id = std::stoull(RequireString(profile, "id"));
            Expect(id >= 2000 && id < 2000 + index && !received[id - 2000],
                "large roster repeated, omitted, or invented an existing player");
            received[id - 2000] = true;
        }
        for (const auto& existing : players)
        {
            Expect(existing->Messages().size() == 1 &&
                existing->Messages().front().Type() == "PlayerJoined" &&
                RequireString(RequireBody(existing->Messages().front()), "id") == std::to_string(2000 + index),
                "a configured-capacity broadcast skipped an existing player");
            existing->ClearMessages();
        }
        player->ClearMessages();
        players.push_back(player);
    }
    const auto overflow = std::make_shared<FakeSession>(3000);
    Expect(Dispatch(fixture.dispatcher, overflow, "Join", JoinBody("CapacityOverflow")).IsOk() &&
        overflow->State() == SessionState::Closing &&
        RequireErrorCode(overflow->Messages().front()) == "server_full",
        "configured capacity did not reject the next joined player");
}

void TestCapacityConstructorRejectsInvalidBounds()
{
    Fixture defaults;
    Expect(defaults.backend->GetPlayerCapacity() == 16 &&
        defaults.backend->GetConnectionCapacity() == 64,
        "explicit capacity support changed the production defaults");
    for (const auto [players, connections] : {std::pair<std::size_t, std::size_t>{0, 64},
        {1, 0}, {65, 64}, {1, Summit::SummitServerBackend::MaximumSessionCapacity + 1}})
    {
        bool rejected = false;
        try
        {
            const Summit::SummitServerBackend invalid(players, connections);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        Expect(rejected, "invalid backend capacity reached storage setup");
    }
}

void TestGameplayLogsAreApprovedEscapedAndExactlyOnce()
{
    // No networking or other test threads run while this scoped global logger is installed.
    ScopedLogCapture capture;
    Fixture fixture;
    const auto player = std::make_shared<FakeSession>(4001);
    const std::string originalName = "한글\"\\닉";
    Join(fixture, player, originalName, 3);
    Expect(capture.logger->lines.size() == 1, "accepted Join did not produce one entry log");
    const auto joined = JsonValue::Parse(capture.logger->lines.front());
    Expect(joined.IsOk() && RequireString(joined.Value(), "event") == "player_joined" &&
        RequireString(joined.Value(), "id") == "4001" &&
        RequireString(joined.Value(), "name") == originalName &&
        *RequireField(joined.Value(), "character").TryUInt64() == 3,
        "entry log did not preserve the approved identity and escaped nickname");

    const auto duplicate = std::make_shared<FakeSession>(4002);
    Expect(Dispatch(fixture.dispatcher, duplicate, "Join", JoinBody(originalName)).IsOk(),
        "duplicate-name rejection failed during logging test");
    const auto failed = std::make_shared<FakeSession>(4003);
    failed->OnDisconnect([&]() { fixture.backend->OnSessionClosed(failed->Id(), Status::Ok()); });
    failed->FailNextSend();
    Expect(!Dispatch(fixture.dispatcher, failed, "Join", JoinBody("FailedAcceptance")).IsOk(),
        "injected acceptance failure was ignored");
    Expect(capture.logger->lines.size() == 1, "rejected or rolled-back Join was logged as membership");

    Expect(Dispatch(fixture.dispatcher, player, "Chat", ChatBody("bad\r\nline")).IsOk(),
        "invalid multiline chat was not handled");
    Expect(capture.logger->lines.size() == 1, "rejected multiline chat entered the accepted chat log");
    const std::string chatText = "안녕 \"친구\" \\ hello";
    Expect(Dispatch(fixture.dispatcher, player, "Chat", ChatBody(chatText)).IsOk(),
        "accepted chat failed during logging test");
    Expect(capture.logger->lines.size() == 2, "accepted chat did not produce exactly one log");
    const auto chat = JsonValue::Parse(capture.logger->lines.back());
    Expect(chat.IsOk() && RequireString(chat.Value(), "event") == "chat" &&
        RequireString(chat.Value(), "id") == "4001" &&
        RequireString(chat.Value(), "name") == originalName &&
        RequireString(chat.Value(), "text") == chatText,
        "chat log used spoofed author fields or failed JSON escaping");
    Expect(Dispatch(fixture.dispatcher, player, "Chat", ChatBody("too soon")).IsOk(),
        "rate-limited chat rejection failed");
    Expect(Dispatch(fixture.dispatcher, player, "PlayerState", PlayerStateBody()).IsOk(),
        "PlayerState failed during logging test");
    const std::string renamed = "새\"닉\\이름";
    Expect(Dispatch(fixture.dispatcher, player, "SetProfile", ProfileBody(renamed, 4)).IsOk(),
        "profile update failed during logging test");
    Expect(capture.logger->lines.size() == 2, "rejected chat, movement, or profile edit added an event log");

    fixture.backend->OnSessionClosed(player->Id(), Status::FailWithoutMessage(ErrorCode::Timeout));
    fixture.backend->OnSessionClosed(player->Id(), Status::FailWithoutMessage(ErrorCode::Closed));
    Expect(capture.logger->lines.size() == 3, "duplicate close produced extra leave logs");
    const auto left = JsonValue::Parse(capture.logger->lines.back());
    Expect(left.IsOk() && RequireString(left.Value(), "event") == "player_left" &&
        RequireString(left.Value(), "id") == "4001" &&
        RequireString(left.Value(), "name") == renamed &&
        *RequireField(left.Value(), "character").TryUInt64() == 4 &&
        *RequireField(left.Value(), "reasonCode").TryUInt64() == static_cast<std::uint64_t>(ErrorCode::Timeout),
        "leave log lost the current approved nickname or first close reason");
    for (const auto& line : capture.logger->lines)
    {
        Expect(line.find_first_of("\r\n") == std::string::npos,
            "one gameplay event split into multiple physical log lines");
    }

    capture.logger->discard = true;
    const auto quiet = std::make_shared<FakeSession>(4004);
    Join(fixture, quiet, "LoggerUnavailable");
    Expect(quiet->State() == SessionState::Authenticated &&
        Dispatch(fixture.dispatcher, quiet, "Chat", ChatBody("still accepted")).IsOk(),
        "discarded log output affected the gameplay result");
    fixture.backend->OnSessionClosed(quiet->Id(), Status::Ok());
    Expect(capture.logger->lines.size() == 3, "discarding logger unexpectedly stored output");
}

void TestConcurrentCloseWaitsForJoinMessagesWithoutBlockingItsCaller()
{
    Fixture fixture;
    const auto leaving = std::make_shared<FakeSession>(301);
    const auto joining = std::make_shared<FakeSession>(302);
    Join(fixture, leaving, "Leaving");

    std::promise<void> sendEntered;
    std::future<void> entered = sendEntered.get_future();
    std::promise<void> releaseSend;
    std::shared_future<void> released = releaseSend.get_future().share();
    joining->BeforeNextSend([&]() {
        sendEntered.set_value();
        released.wait();
    });
    Status joinStatus = Status::Ok();
    std::exception_ptr joinFailure;
    std::thread joiningThread([&]() {
        try
        {
            joinStatus = Dispatch(fixture.dispatcher, joining, "Join", JoinBody("Joining"));
        }
        catch (...)
        {
            joinFailure = std::current_exception();
        }
    });
    const bool blockedInSend = entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!blockedInSend)
    {
        releaseSend.set_value();
        joiningThread.join();
        if (joinFailure) std::rethrow_exception(joinFailure);
        Expect(false, "Join did not reach the blocked acceptance Send");
    }

    std::promise<void> closeReturned;
    std::future<void> closed = closeReturned.get_future();
    std::thread closingThread([&]() {
        leaving->Disconnect(Status::FailWithoutMessage(ErrorCode::Closed));
        fixture.backend->OnSessionClosed(leaving->Id(), Status::FailWithoutMessage(ErrorCode::Closed));
        closeReturned.set_value();
    });
    const bool returnedWithoutWaiting =
        closed.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    // Release before asserting, so a lock-across-Send regression fails cleanly
    // rather than leaving a thread permanently blocked in this test.
    releaseSend.set_value();
    closingThread.join();
    joiningThread.join();
    if (joinFailure) std::rethrow_exception(joinFailure);
    Expect(returnedWithoutWaiting, "off-runner close waited on an active Session::Send");
    Expect(joinStatus.IsOk(), "Join failed during an unrelated session close");
    CompleteDirectory(fixture, joining);

    const auto& messages = joining->Messages();
    Expect(messages.size() == 2, "joining peer did not receive acceptance and subsequent leave");
    Expect(messages[0].Type() == "JoinAccepted", "a concurrent leave overtook JoinAccepted");
    Expect(messages[1].Type() == "PlayerLeft" && RequireString(RequireBody(messages[1]), "id") == "301",
        "concurrent close lost its tombstone or used the wrong ID");
    const auto directory = DirectoryProfiles(*joining);
    Expect(directory.size() <= 1, "concurrent close invented directory entries");
    for (const auto& profile : directory)
        Expect(RequireString(profile, "id") == "301", "directory contained an unrelated identity");
    // A paged snapshot may include the just-closed identity. The live tombstone remains
    // mandatory, so the client can suppress stale directory pages through its watermark.
    const auto replacement = std::make_shared<FakeSession>(303);
    Join(fixture, replacement, "Leaving");
    Expect(replacement->State() == SessionState::Authenticated, "deferred close did not release nickname");
}

void TestSynchronousCloseDuringSendPreservesProfileOrder()
{
    Fixture fixture;
    const auto sender = std::make_shared<FakeSession>(311);
    const auto receiver = std::make_shared<FakeSession>(312);
    const auto leaving = std::make_shared<FakeSession>(313);
    Join(fixture, sender, "Sender");
    Join(fixture, receiver, "Receiver");
    Join(fixture, leaving, "Leaving");
    sender->ClearMessages();
    receiver->ClearMessages();
    leaving->ClearMessages();
    receiver->BeforeNextSend([&]() {
        leaving->Disconnect(Status::FailWithoutMessage(ErrorCode::Closed));
        fixture.backend->OnSessionClosed(leaving->Id(), Status::FailWithoutMessage(ErrorCode::Closed));
    });
    JsonValue::Object profile;
    profile.emplace("name", JsonValue(std::string("SenderChanged")));
    profile.emplace("c", JsonValue(std::uint64_t{1}));
    const Status changed = Dispatch(fixture.dispatcher, sender, "SetProfile", JsonValue(std::move(profile)));
    Expect(changed.IsOk(), "profile change failed with reentrant close");
    for (const auto& session : {sender, receiver})
    {
        Expect(session->Messages().size() == 2, "reentrant close lost profile or leave event");
        Expect(session->Messages()[0].Type() == "ProfileChanged" &&
                session->Messages()[1].Type() == "PlayerLeft",
            "reentrant close interrupted an in-progress profile broadcast");
    }
}

void TestFullConnectionSetCanCloseReentrantlyDuringExpiration(
    const std::size_t capacity, const bool collide = false)
{
    Fixture fixture(capacity, capacity);
    std::vector<std::shared_ptr<FakeSession>> pending;
    pending.reserve(capacity);
    std::size_t closeCallbacks = 0;
    std::uint64_t nextId = 400;
    for (std::size_t index = 0; index < capacity; ++index)
    {
        // std::hash<uint64_t> need not be the identity on this standard library.
        // Pick actual colliding keys rather than assuming an arithmetic stride collides.
        while (collide && std::hash<std::uint64_t>{}(nextId) % capacity != 0) ++nextId;
        auto session = std::make_shared<FakeSession>(nextId++);
        fixture.backend->OnSessionOpened(session);
        const SessionId id = session->Id();
        session->OnDisconnect([&, id]() {
            ++closeCallbacks;
            fixture.backend->OnSessionClosed(id, Status::FailWithoutMessage(ErrorCode::Closed));
            // Core reports each close once; duplicates still must not consume
            // extra fixed-queue entries or generate a second leave.
            fixture.backend->OnSessionClosed(id, Status::FailWithoutMessage(ErrorCode::Closed));
        });
        pending.push_back(std::move(session));
    }
    fixture.backend->ExpireUnjoinedSessions((std::numeric_limits<std::uint64_t>::max)());
    Expect(closeCallbacks == capacity,
        "the full pending connection set did not close");
    for (const auto& session : pending)
    {
        Expect(session->State() == SessionState::Closed, "expiration left a pending session connected");
    }
    // Exercise ownership release after the full queue drained, including the
    // ring cursor wrapping back to its original position.
    const auto replacement = std::make_shared<FakeSession>(500);
    fixture.backend->OnSessionOpened(replacement);
    Join(fixture, replacement, "AfterExpiration");
    Expect(replacement->State() == SessionState::Authenticated, "close drain stranded operation ownership");
    replacement->OnDisconnect([&]() {
        fixture.backend->OnSessionClosed(replacement->Id(), Status::Ok());
        fixture.backend->OnSessionClosed(replacement->Id(), Status::Ok());
    });
    const auto expired = std::make_shared<FakeSession>(500 + capacity);
    fixture.backend->OnSessionOpened(expired);
    expired->OnDisconnect([&]() {
        fixture.backend->OnSessionClosed(expired->Id(), Status::Ok());
        replacement->Disconnect(Status::Ok());
    });
    fixture.backend->ExpireUnjoinedSessions((std::numeric_limits<std::uint64_t>::max)());
    const auto afterWrap = std::make_shared<FakeSession>(700 + capacity);
    Join(fixture, afterWrap, "AfterExpiration");
    Expect(afterWrap->State() == SessionState::Authenticated,
        "reused hash-chain slots retained an old close or nickname");
}
}

int main()
{
    try
    {
        TestPlayerSnapshotContainsOnlyCommittedOwnedProfiles();
        TestMembershipNoticesDescribeActualCommitsOnly();
        TestAnnouncementsValidateOperatorTextAndHaveNoHistory();
        TestNoticeSendFailuresAndReentrantClosePreserveEventOrder();
        TestConcurrentCloseWaitsForJoinMessagesWithoutBlockingItsCaller();
        TestSynchronousCloseDuringSendPreservesProfileOrder();
        TestFullConnectionSetCanCloseReentrantlyDuringExpiration(Summit::SummitServerBackend::ConnectionCapacity);
        // These 65,536 fake sessions exercise startup storage and an allocation-free close
        // burst, not 65,536 all-to-all game joins. The latter has a separate quadratic cost.
        TestFullConnectionSetCanCloseReentrantlyDuringExpiration(Summit::SummitServerBackend::MaximumSessionCapacity);
        TestFullConnectionSetCanCloseReentrantlyDuringExpiration(4, true);
        TestCapacityConstructorRejectsInvalidBounds();
        TestConfiguredCapacityAndLargeRosterStayCompatible();
        TestGameplayLogsAreApprovedEscapedAndExactlyOnce();
        TestProfilesAreAtomicAndAuthoritative();
        TestAllOrdinaryCharactersCanJoinAndChangeProfile();
        TestNicknameCharacterIsAutomaticAndAuthoritative();
        TestChatIsAuthoritativeEchoedAndRateLimitedWithoutHistory();
        TestChatValidationRetainsConnectionAndDoesNotConsumeCooldown();
        TestChatSendFailuresPreserveOtherRecipientsAndMembershipOrder();
        TestDuplicateNameCanRetryOnSameConnection();
        TestPlayerStateIsCoalescedWithoutEchoAndSeedsNewVisibility();
        TestLeaveReleasesNickname();
        TestInvalidJoinAndPreJoinStatePolicies();
        TestMembershipDeliveryFailureDisconnectsButStateDropDoesNot();
        TestMaximumSessionIdUsesDecimalString();
        TestJoinExceptionRollsBackIdentityWithoutPhantomLeave();
        TestUnjoinedSessionExpiresOnAbsoluteDeadline();
        TestCapacityRejectsOnlyTheSeventeenthPlayer();
        std::cout << "SummitServer backend tests passed.\n";
        return 0;
    }
    catch (const std::exception& failure)
    {
        std::cerr << "SummitServer backend test failed: " << failure.what() << '\n';
        return 1;
    }
}
