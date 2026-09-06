#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include "ServerCore/Protocol/FrameCodec.h"
#include "ServerCore/Protocol/DatagramCodec.h"
#include "ServerCore/Protocol/Message.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
using Json = ServerCore::Protocol::JsonValue;
using Clock = std::chrono::steady_clock;
namespace Codec = ServerCore::Protocol::FrameCodec;
namespace Datagram = ServerCore::Protocol::DatagramCodec;
constexpr std::size_t MaximumAttempts = 262144;
constexpr std::size_t MaximumErrorSamples = 20;
constexpr std::size_t MaximumLatencySamples = 100000;
constexpr std::size_t StateHistorySize = 256;
std::atomic<bool> gStop{ false };

BOOL WINAPI ConsoleControl(const DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT)
    {
        gStop.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

double Seconds(const Clock::time_point time)
{
    return std::chrono::duration<double>(time.time_since_epoch()).count();
}

struct Options
{
    std::string address = "127.0.0.1";
    std::uint16_t port = 17891;
    std::size_t clients = 1000;
    double ramp = 250.0;
    double hold = 20.0;
    double timeout = 180.0;
    double connectTimeout = 10.0;
    double joinTimeout = 30.0;
    double idleTimeout = 30.0;
    double rate = 20.0;
    bool perClientRate = false;
    double churnRate = 5.0;
    std::uint64_t seed = 1;
    std::uint32_t maxFrame = Codec::DefaultMaxBodySize;
    std::string scenario = "connect";
    std::string layout = "grid";
    std::string movementTransport = "udp";
    double spacing = 8.0;
    std::size_t columns = 32;
    std::filesystem::path output = "SummitLoadTest-result.json";
    bool selfCheck = false;
    bool help = false;
};

std::uint64_t ParseUnsigned(const std::string_view text)
{
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        throw std::runtime_error("Expected an unsigned integer: " + std::string(text));
    }
    return value;
}

double ParseNumber(const std::string& text)
{
    std::size_t used = 0;
    const double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value))
    {
        throw std::runtime_error("Expected a finite number: " + text);
    }
    return value;
}

std::string Prompt(const char* label, const std::string& fallback)
{
    std::cout << label << " [" << fallback << "]: " << std::flush;
    std::string input;
    if (!std::getline(std::cin, input))
    {
        throw std::runtime_error("Input ended. Use --help for non-interactive arguments.");
    }
    return input.empty() ? fallback : input;
}

void PrintHelp()
{
    std::cout <<
        "SummitLoadTest -- local Summit protocol v6 load client (Windows)\n"
        "No arguments: prompt for address, port, clients, hold and scenario.\n"
        "Start SummitServer separately on 127.0.0.1:17891 with suitable capacity.\n"
        "The game normally uses port 17890; this tool defaults to TEST port 17891.\n\n"
        "  --address 127.0.0.1       Explicit IPv4 target (default: local test server)\n"
        "  --port 17891             Test server port\n"
        "  --clients 1000           Target concurrent sockets, 1..65536\n"
        "  --scenario connect      connect/transport | join/joined | idle | churn\n"
        "  --ramp 250               Initial connection attempts per second\n"
        "  --hold 20                Seconds AFTER initial attempts settle\n"
        "  --rate 20                Aggregate PlayerState attempts per second\n"
        "  --per-client-rate 0.1    Alternative rate per joined client\n"
        "  --layout grid           grid | crowd | isolated (fixed AOI positions)\n"
        "  --spacing 8             Grid spacing in world units, 1..128\n"
        "  --columns 32            Grid columns, 1..65536\n"
        "  --movement-transport udp  udp (negotiate) | tcp (frozen-baseline comparison)\n"
        "  --churn-rate 5           Replacement connections/sec during churn hold\n"
        "  --seed 1                 Reproducible slot order and latency reservoir\n"
        "  --timeout 180            Entire workload deadline, including ramp\n"
        "  --connect-timeout 10     TCP handshake deadline\n"
        "  --join-timeout 30        JoinAccepted deadline after TCP connects\n"
        "  --idle-timeout 30        Expected minimum server close age in idle mode\n"
        "  --max-frame 65536        Receive body cap; must be below 1 MiB\n"
        "  --output result.json     JSON summary (overwrites this file)\n"
        "  --self-check             Framing/JSON/validation checks; opens NO sockets\n\n"
        "connect keeps TCP sockets without Join; idle additionally requires server\n"
        "timeout cleanup. join sends Join and states; churn replaces joined sockets.\n"
        "AOI sends state batches only to nearby peers. Crowd intentionally keeps\n"
        "everyone at spawn; isolated has no expected relays. No hidden heartbeat\n"
        "is sent on legacy TCP. UDP peers send TCP Heartbeat every 5 seconds; UDP\n"
        "Hello must receive UdpReady before hold or movement. Missing UDP advertisement\n"
        "falls back to TCP. Each negotiated peer owns one additional UDP socket.\n"
        "Global profile/notice traffic still reaches all peers. Progress is once per second.\n"
        "Exit: 0 passed, 1 failed checks/load, 2 invalid options, 130 cancelled.\n";
}

Options ParseOptions(const int argc, char* argv[])
{
    Options options;
    if (argc == 1)
    {
        std::cout << "SummitServer 부하 테스트 — 먼저 서버를 실행해 주세요. 기본 테스트 포트는 17891입니다.\n";
        options.address = Prompt("서버 IPv4 주소", options.address);
        const auto port = ParseUnsigned(Prompt("포트", "17891"));
        if (port == 0 || port > 65535) throw std::runtime_error("Port must be 1..65535");
        options.port = static_cast<std::uint16_t>(port);
        options.clients = static_cast<std::size_t>(ParseUnsigned(Prompt("동시 연결 수", "1000")));
        options.hold = ParseNumber(Prompt("연결 완료 후 유지 시간(초)", "20"));
        options.scenario = Prompt("종류 (connect: 연결만 / join: 가입과 이동 / idle: 유휴 정리 / churn: 재접속)", "connect");
        if (options.scenario == "join" || options.scenario == "joined" || options.scenario == "churn")
        {
            options.layout = Prompt("배치 (grid: 분산 / isolated: 서로 안 보임 / crowd: 시작점 밀집)", "grid");
            options.rate = ParseNumber(Prompt("각 클라이언트의 초당 상태 전송 수", "20"));
            options.perClientRate = true;
        }
    }
    bool explicitRate = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string key = argv[index];
        if (key == "--help" || key == "-h") { options.help = true; continue; }
        if (key == "--self-check") { options.selfCheck = true; continue; }
        if (++index >= argc) throw std::runtime_error("Missing value for " + key);
        const std::string value = argv[index];
        if (key == "--address") options.address = value;
        else if (key == "--port")
        {
            const auto port = ParseUnsigned(value);
            if (port == 0 || port > 65535) throw std::runtime_error("Port must be 1..65535");
            options.port = static_cast<std::uint16_t>(port);
        }
        else if (key == "--clients") options.clients = static_cast<std::size_t>(ParseUnsigned(value));
        else if (key == "--scenario") options.scenario = value;
        else if (key == "--layout") options.layout = value;
        else if (key == "--movement-transport") options.movementTransport = value;
        else if (key == "--spacing") options.spacing = ParseNumber(value);
        else if (key == "--columns") options.columns = static_cast<std::size_t>(ParseUnsigned(value));
        else if (key == "--ramp") options.ramp = ParseNumber(value);
        else if (key == "--hold") options.hold = ParseNumber(value);
        else if (key == "--timeout") options.timeout = ParseNumber(value);
        else if (key == "--connect-timeout") options.connectTimeout = ParseNumber(value);
        else if (key == "--join-timeout") options.joinTimeout = ParseNumber(value);
        else if (key == "--idle-timeout") options.idleTimeout = ParseNumber(value);
        else if (key == "--rate" || key == "--per-client-rate")
        {
            if (explicitRate) throw std::runtime_error("Choose only one rate argument");
            explicitRate = true;
            options.rate = ParseNumber(value);
            options.perClientRate = key == "--per-client-rate";
        }
        else if (key == "--churn-rate") options.churnRate = ParseNumber(value);
        else if (key == "--seed") options.seed = ParseUnsigned(value);
        else if (key == "--max-frame")
        {
            const auto limit = ParseUnsigned(value);
            if (limit == 0 || limit >= 1024 * 1024) throw std::runtime_error("Frame cap must be 1..1048575");
            options.maxFrame = static_cast<std::uint32_t>(limit);
        }
        else if (key == "--output") options.output = std::filesystem::path(value);
        else throw std::runtime_error("Unknown argument: " + key);
    }
    IN_ADDR address{};
    if (InetPtonA(AF_INET, options.address.c_str(), &address) != 1)
    {
        throw std::runtime_error("An explicit valid IPv4 address is required");
    }
    if (options.scenario == "transport") options.scenario = "connect";
    if (options.scenario == "joined") options.scenario = "join";
    if (options.movementTransport != "udp" && options.movementTransport != "tcp")
        throw std::runtime_error("Movement transport must be udp or tcp");
    if (options.clients == 0 || options.clients > 65536 || options.ramp <= 0 ||
        options.ramp > 100000 || options.hold < 0 || options.hold > 3600 ||
        options.timeout <= 0 || options.timeout > 7200 || options.connectTimeout <= 0 ||
        options.joinTimeout <= 0 || options.idleTimeout <= 0 || options.rate < 0 ||
        options.rate > 100000 || options.churnRate <= 0 || options.churnRate > 10000 ||
        options.spacing < 1 || options.spacing > 128 || options.columns < 1 || options.columns > 65536)
    {
        throw std::runtime_error("Invalid count/rate/deadline; see --help");
    }
    if (options.scenario != "connect" && options.scenario != "join" &&
        options.scenario != "idle" && options.scenario != "churn")
    {
        throw std::runtime_error("Scenario must be connect, join, idle, or churn");
    }
    if (options.layout != "grid" && options.layout != "crowd" && options.layout != "isolated")
        throw std::runtime_error("Layout must be grid, crowd, or isolated");
    if (options.layout == "grid" &&
        (static_cast<double>(options.columns) * options.spacing > 1'000'000.0 ||
            static_cast<double>(options.clients / options.columns + 1) * options.spacing > 1'000'000.0))
        throw std::runtime_error("Grid exceeds the server's supported world-coordinate range");
    return options;
}

// Coordinates remain fixed, so expected AOI membership can be checked without
// confusing in-flight movement with a visibility bug. Sequence numbers are sent
// separately in q; churn reuses the departed connection slot's position.
std::pair<double, double> Position(const Options& options, const std::size_t slot)
{
    if (options.layout == "crowd") return { 2.5, 4.0 };
    const auto columns = options.layout == "isolated" ? std::size_t{256} : options.columns;
    const double spacing = options.layout == "isolated" ? 128.0 : options.spacing;
    return { (static_cast<double>(slot % columns) - static_cast<double>(columns / 2)) * spacing,
        (static_cast<double>(slot / columns) - static_cast<double>((options.clients / columns) / 2)) * spacing };
}

struct Latencies
{
    std::uint64_t count = 0;
    std::vector<double> samples;
    std::mt19937_64 random;
    explicit Latencies(const std::uint64_t seed) : random(seed) {}
    void Add(const double milliseconds)
    {
        ++count;
        if (samples.size() < MaximumLatencySamples) samples.push_back(milliseconds);
        else
        {
            const auto index = std::uniform_int_distribution<std::uint64_t>(0, count - 1)(random);
            if (index < samples.size()) samples[static_cast<std::size_t>(index)] = milliseconds;
        }
    }
    Json ToJson() const
    {
        auto ordered = samples;
        std::sort(ordered.begin(), ordered.end());
        const auto percentile = [&](const double p) -> Json {
            if (ordered.empty()) return Json();
            const auto index = static_cast<std::size_t>(std::ceil(p * static_cast<double>(ordered.size()))) - 1;
            return Json(ordered[index]);
        };
        return Json(Json::Object{ {"observations", Json(count)}, {"samples", Json(samples.size())},
            {"p50_ms", percentile(0.50)}, {"p95_ms", percentile(0.95)}, {"p99_ms", percentile(0.99)} });
    }
};

const Json& Field(const Json& body, const std::string_view key)
{
    const Json* value = body.Find(key);
    if (!value) throw std::runtime_error("Missing field: " + std::string(key));
    return *value;
}

std::uint64_t Unsigned(const Json& value)
{
    if (const auto* number = value.TryUInt64()) return *number;
    if (const auto* number = value.TryInt64(); number && *number >= 0) return static_cast<std::uint64_t>(*number);
    throw std::runtime_error("Expected a nonnegative integer field");
}

std::uint64_t SessionId(const Json& body)
{
    const auto* text = Field(body, "id").TryString();
    if (!text) throw std::runtime_error("Session ID must be a decimal string");
    const auto id = ParseUnsigned(*text);
    if (id == 0 || std::to_string(id) != *text) throw std::runtime_error("Invalid session ID");
    return id;
}

std::uint64_t StateRevision(const Json& body, const bool required)
{
    const auto* value = body.Find("r");
    if (!value)
    {
        if (required) throw std::runtime_error("UDP state/Enter omitted its revision");
        return 0;
    }
    const auto* text = value->TryString();
    if (!text) throw std::runtime_error("State revision must be a canonical decimal string");
    const auto revision = ParseUnsigned(*text);
    if (std::to_string(revision) != *text) throw std::runtime_error("State revision is not canonical");
    return revision;
}

std::vector<std::byte> Encode(const std::string_view type, const Json& body)
{
    auto json = ServerCore::Protocol::SerializeMessage(type, body);
    if (!json.IsOk()) throw std::runtime_error("Could not serialize load message");
    std::vector<std::byte> frame(json.Value().size() + Codec::HeaderSize);
    if (!Codec::EncodeTo(json.Value(), frame).IsOk()) throw std::runtime_error("Could not encode load frame");
    return frame;
}

// Allocate only the declared body, after validating its prefix. Idle connections
// therefore do not reserve maxFrame bytes each; no completed-frame queue exists.
struct Framer
{
    std::array<std::byte, Codec::HeaderSize> header{};
    std::size_t headerUsed = 0;
    std::vector<std::byte> body;
    std::size_t bodyUsed = 0;

    bool Incomplete() const { return headerUsed != 0; }

    template <typename Callback>
    void Feed(std::span<const std::byte> input, const std::uint32_t maximum, Callback&& callback)
    {
        while (!input.empty())
        {
            if (headerUsed < header.size())
            {
                const auto count = std::min(header.size() - headerUsed, input.size());
                std::memcpy(header.data() + headerUsed, input.data(), count);
                headerUsed += count;
                input = input.subspan(count);
                if (headerUsed != header.size()) continue;
                std::uint32_t length = 0;
                for (std::size_t index = 0; index < header.size(); ++index)
                    length |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(header[index])) << (index * 8);
                if (length == 0 || length > maximum) throw std::runtime_error("Receive frame length outside configured cap");
                body.resize(length);
                bodyUsed = 0;
            }
            const auto count = std::min(body.size() - bodyUsed, input.size());
            std::memcpy(body.data() + bodyUsed, input.data(), count);
            bodyUsed += count;
            input = input.subspan(count);
            if (bodyUsed == body.size())
            {
                callback(std::span<const std::byte>(body));
                headerUsed = 0;
                bodyUsed = 0;
                // Release one-off large rosters instead of retaining their peak on every socket.
                if (body.capacity() > 4096) std::vector<std::byte>{}.swap(body);
            }
        }
    }
};

enum class Stage { Closed, Connecting, Connected, Joining, Joined };
struct VisibleState
{
    std::uint64_t sequence = 0;
    std::uint64_t serverTime = 0;
    std::uint64_t initialSequence = 0;
    double enteredAt = 0;
    double lastReceiveAt = 0;
    double sourceSendTime = 0;
    bool receivedUpdate = false;
    std::uint64_t serverRevision = 0;
};
struct Connection
{
    SOCKET socket = INVALID_SOCKET;
    SOCKET udpSocket = INVALID_SOCKET;
    Datagram::Token udpToken{};
    std::uint64_t udpSendSequence = 0, udpLargestReceivedSequence = 0;
    bool udpAdvertised = false, udpReady = false;
    double nextUdpHello = 0, nextHeartbeat = 0;
    Stage stage = Stage::Closed;
    std::size_t attempt = 0;
    double started = 0;
    double connected = 0;
    double sendStarted = 0;
    Framer receive;
    std::vector<std::byte> send;
    std::size_t sent = 0;
    bool pendingState = false;
    bool pendingAck = false;
    bool pendingHeartbeat = false;
    std::vector<std::byte> nextAck;
    bool directoryReady = false;
    bool visibilityReady = false;
    std::uint64_t directoryThrough = 0;
    std::uint64_t directoryCursor = 0;
    // Dense attempt ordinals avoid an unordered_map<string,profile> on every peer.
    // Membership still costs O(N^2) bits; the real protocol broadcasts O(N^2) joins.
    std::vector<std::uint64_t> known;
    std::unordered_map<std::size_t, VisibleState> visible;
};

struct Attempt
{
    std::string name;
    std::uint64_t id = 0;
    std::uint64_t statesIssued = 0;
    unsigned int character = 0;
    std::uint64_t lastStateSentSerial = 0;
    double lastStateSentTime = 0;
    double firstStateSentTime = 0;
    double x = 0;
    double y = 0;
    bool active = true;
    // Allocate a bounded history only for senders; never an unbounded per-state log.
    std::vector<std::pair<std::uint64_t, double>> history;
};

double HistoryTime(const Attempt& sender, const std::uint64_t sequence)
{
    if (sender.history.empty()) return 0;
    const auto& entry = sender.history[sequence % StateHistorySize];
    return entry.first == sequence ? entry.second : 0;
}

bool MissingStateUpdates(const Attempt& sender, const VisibleState& visible, const double now)
{
    if (!sender.active || visible.receivedUpdate || now - visible.enteredAt < 1.0 ||
        sender.lastStateSentSerial <= visible.initialSequence) return false;
    // Sequence numbers can have gaps when UDP send would block. Inspect the bounded
    // successful-send history rather than assuming its oldest ordinal was sent.
    // A source whose only newer sends are less than a second old still gets grace.
    const std::uint64_t oldestRetained = sender.lastStateSentSerial >= StateHistorySize
        ? sender.lastStateSentSerial - StateHistorySize + 1 : 1;
    return std::any_of(sender.history.begin(), sender.history.end(), [&](const auto& entry) {
        return entry.first > visible.initialSequence && entry.first >= oldestRetained &&
            entry.first <= sender.lastStateSentSerial && entry.second != 0 && now - entry.second >= 1.0;
    });
}

class JoinRejection final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class LoadRun
{
public:
    explicit LoadRun(Options options)
        : mOptions(std::move(options)), mClients(mOptions.clients), mRandom(mOptions.seed),
          mConnectLatency(mOptions.seed + 1), mJoinLatency(mOptions.seed + 2), mIdleLatency(mOptions.seed + 3),
          mRelayLatency(mOptions.seed + 4), mHoldRelayLatency(mOptions.seed + 5),
          mFinalSourceAge(mOptions.seed + 6), mFinalReceiveAge(mOptions.seed + 7)
    {
        mAddress.sin_family = AF_INET;
        mAddress.sin_port = htons(mOptions.port);
        if (InetPtonA(AF_INET, mOptions.address.c_str(), &mAddress.sin_addr) != 1)
            throw std::runtime_error("Invalid IPv4 address");
        mRunPrefix = "L" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-";
        mOrder.resize(mClients.size());
        for (std::size_t index = 0; index < mOrder.size(); ++index) mOrder[index] = index;
        std::shuffle(mOrder.begin(), mOrder.end(), mRandom);
    }

    ~LoadRun()
    {
        for (auto& client : mClients)
        {
            if (client.socket != INVALID_SOCKET) closesocket(client.socket);
            if (client.udpSocket != INVALID_SOCKET) closesocket(client.udpSocket);
        }
    }

    int Run()
    {
        const double start = Seconds(Clock::now());
        mExposureUpdatedAt = start;
        mMeasuring = true;
        mWorkDeadline = start + mOptions.timeout;
        double holdStarted = 0;
        double nextState = start;
        double nextChurn = start;
        double nextProgress = start;
        std::uint64_t holdStatesStart = 0;
        std::uint64_t holdInitialStatesStart = 0;
        std::uint64_t holdBatchFramesStart = 0;
        std::uint64_t holdBatchItemsStart = 0;
        std::uint64_t holdStateUpdatesStart = 0;
        std::uint64_t holdEntersStart = 0;
        std::size_t launched = 0;
        std::size_t cursor = 0;
        bool timedOut = false;
        std::cout << "Target " << mOptions.address << ':' << mOptions.port << " (TEST port default 17891), scenario="
                  << mOptions.scenario << ", requested=" << mOptions.clients << ", hold=" << mOptions.hold
                  << "s after ramp. Ctrl+C cancels.\n";
        try
        {
        while (!gStop.load(std::memory_order_relaxed))
        {
            const double now = Seconds(Clock::now());
            AccumulateExposure(now);
            if (now - start >= mOptions.timeout) { timedOut = true; break; }
            if (holdStarted != 0 && now - holdStarted >= mOptions.hold) break;
            // A bounded catch-up batch keeps a slow client machine from issuing a sudden connect storm.
            for (std::size_t burst = 0; burst < 64 && launched < mClients.size() &&
                now >= start + static_cast<double>(launched) / mOptions.ramp; ++burst)
            {
                Open(mClients[mOrder[launched++]], now);
            }
            if (launched == mClients.size() && holdStarted == 0 && mConnecting == 0 && mJoining == 0 &&
                mSynchronizing == 0 && mUdpPending == 0)
            {
                holdStarted = Seconds(Clock::now());
                AccumulateExposure(holdStarted);
                mHoldStartedAt = holdStarted;
                holdStatesStart = mCounts["states_sent"];
                holdInitialStatesStart = mCounts["initial_states_sent"];
                holdBatchFramesStart = mCounts["state_batch_frames"];
                holdBatchItemsStart = mCounts["state_batch_items_received"];
                holdStateUpdatesStart = mCounts["state_updates_received"];
                holdEntersStart = mCounts["visibility_enters_received"];
                nextChurn = now;
                mCounts["connected_at_hold_start"] = mActive;
                mCounts["joined_at_hold_start"] = mJoined;
                mCounts["ramp_settled_ms"] = static_cast<std::uint64_t>((now - start) * 1000.0);
            }
            if (mOptions.scenario == "churn" && holdStarted != 0 && now >= nextChurn)
            {
                auto& client = mClients[mOrder[mChurnCursor++ % mOrder.size()]];
                if (client.stage != Stage::Connecting && client.stage != Stage::Joining)
                {
                    Close(client, "intentional_churn_closes");
                    Open(client, now);
                    ++mCounts["churn_replacements"];
                }
                nextChurn = now + 1.0 / mOptions.churnRate;
            }
            const double stateRate = mOptions.rate * (mOptions.perClientRate ? static_cast<double>(mJoined) : 1.0);
            if (stateRate > 0 && mJoined != 0 && now >= nextState)
            {
                const double interval = 1.0 / stateRate;
                std::size_t budget = 512;
                while (budget-- > 0 && nextState <= now)
                {
                    for (std::size_t inspected = 0; inspected < mClients.size(); ++inspected)
                    {
                        auto& client = mClients[mOrder[cursor++ % mOrder.size()]];
                        if (client.stage == Stage::Joined) { SendState(client, now); break; }
                    }
                    nextState += interval;
                }
                if (nextState <= now)
                {
                    mCounts["state_schedule_missed"] += static_cast<std::uint64_t>((now - nextState) * stateRate) + 1;
                    nextState = now + interval;
                }
            }
            else if (mJoined == 0) nextState = now;

            for (auto& client : mClients)
            {
                if (client.stage != Stage::Joined || !client.udpAdvertised) continue;
                if (!client.udpReady && now >= client.nextUdpHello) SendUdpHello(client, now);
                if (client.stage != Stage::Joined) continue;
                if (now >= client.nextHeartbeat && client.send.empty() && client.nextAck.empty())
                {
                    client.send = Encode("Heartbeat", Json(Json::Object{}));
                    client.sent = 0;
                    client.pendingHeartbeat = true;
                    client.sendStarted = now;
                    client.nextHeartbeat = now + 5.0;
                }
            }
            Poll();
            const double afterPoll = Seconds(Clock::now());
            for (auto& client : mClients)
            {
                if (client.stage == Stage::Connecting && afterPoll - client.started > mOptions.connectTimeout)
                    Fail(client, "connect_timeout", "TCP handshake deadline");
                else if (client.stage == Stage::Joining && afterPoll - client.connected > mOptions.joinTimeout)
                    Fail(client, "join_timeout", "JoinAccepted deadline");
                else if (client.stage == Stage::Joined && !client.directoryReady &&
                    afterPoll - client.connected > mOptions.joinTimeout)
                    Fail(client, "directory_timeout", "Initial directory completion deadline");
                else if (client.stage == Stage::Joined && client.udpAdvertised && !client.udpReady &&
                    afterPoll - client.connected > mOptions.joinTimeout)
                    Fail(client, "udp_ready_timeout", "Advertised UDP never returned UdpReady");
                else if (!client.send.empty() && afterPoll - client.sendStarted > mOptions.connectTimeout)
                    Fail(client, "send_timeout", "Client send queue did not drain");
            }
            if (afterPoll >= nextProgress)
            {
                std::cout << std::fixed << std::setprecision(1) << "t=" << afterPoll - start
                    << "s attempted=" << mAttempts.size() << " connected=" << mActive
                    << " joined=" << mJoined << " frames=" << mCounts["frames_received"]
                    << " errors=" << mCounts["errors"] << " unexpected_close="
                    << mCounts["unexpected_disconnects"] << '\n' << std::flush;
                nextProgress = afterPoll + 1.0;
            }
        }
        }
        catch (const std::exception& error)
        {
            Error("runner_error", error.what());
        }
        const double measurementEnded = Seconds(Clock::now());
        AccumulateExposure(measurementEnded);
        mMeasuring = false;
        const bool cancelled = gStop.load(std::memory_order_relaxed);
        mCounts["connected_before_cleanup"] = mActive;
        mCounts["joined_before_cleanup"] = mJoined;
        mCounts["udp_ready_before_cleanup"] = mUdpReady;
        mCounts["udp_pending_before_cleanup"] = mUdpPending;
        if (mUdpPending != 0) Error("udp_readiness_incomplete", "An advertised UDP peer never completed its handshake");
        if (mOptions.scenario == "idle")
        {
            mCounts["idle_survivors"] = mActive;
            if (mActive != 0) Error("idle_survivors", "Hold ended before all unjoined connections were closed by server");
        }
        if (timedOut) Error("global_timeout", "Overall deadline reached");
        for (const auto* name : {"missing_state_updates", "final_visible_source_age_history_missing",
            "final_visible_never_updated_edges", "final_visible_departed_in_flight"})
            mCounts.try_emplace(name, 0);
        if (mOptions.scenario == "join" || mOptions.scenario == "churn")
            CheckFinalVisibility(mOptions.scenario == "join", measurementEnded);
        const auto holdStatesSent = holdStarted != 0 ? mCounts["states_sent"] - holdStatesStart : 0;
        const auto holdInitialStates = holdStarted != 0 ? mCounts["initial_states_sent"] - holdInitialStatesStart : 0;
        const auto holdBatchFrames = holdStarted != 0 ? mCounts["state_batch_frames"] - holdBatchFramesStart : 0;
        const auto holdBatchItems = holdStarted != 0 ? mCounts["state_batch_items_received"] - holdBatchItemsStart : 0;
        const auto holdStateUpdates = holdStarted != 0 ? mCounts["state_updates_received"] - holdStateUpdatesStart : 0;
        const auto holdEnters = holdStarted != 0 ? mCounts["visibility_enters_received"] - holdEntersStart : 0;
        Cleanup();
        const bool passed = !cancelled && !timedOut && mCounts["errors"] == 0 &&
            mCounts["unexpected_disconnects"] == 0 && mCounts["join_rejected"] == 0 &&
            mCounts["connect_succeeded"] >= mOptions.clients;
        Json::Object counts;
        for (const auto& [name, count] : mCounts) counts.emplace(name, Json(count));
        Json::Array errors;
        for (const auto& message : mErrors) errors.emplace_back(Json(message));
        Json::Object report{
            {"report_version", Json(4)}, {"protocol_schema", Json(6)}, {"passed", Json(passed)},
            {"cancelled", Json(cancelled)}, {"timed_out", Json(timedOut)},
            {"target", Json(mOptions.address + ":" + std::to_string(mOptions.port))},
            {"scenario", Json(mOptions.scenario)}, {"requested_clients", Json(mOptions.clients)},
            {"seed", Json(mOptions.seed)}, {"run_name_prefix", Json(mRunPrefix)},
            {"ramp_per_second", Json(mOptions.ramp)}, {"hold_seconds", Json(mOptions.hold)},
            {"rate", Json(mOptions.rate)}, {"rate_is_per_client", Json(mOptions.perClientRate)},
            {"layout", Json(mOptions.layout)}, {"grid_spacing", Json(mOptions.spacing)},
            {"requested_movement_transport", Json(mOptions.movementTransport)},
            {"grid_columns", Json(mOptions.columns)},
            {"hold_states_sent", Json(holdStatesSent)},
            {"hold_initial_states_sent", Json(holdInitialStates)},
            {"hold_periodic_states_sent", Json(holdStatesSent - holdInitialStates)},
            {"hold_actual_seconds", Json(holdStarted != 0 ? measurementEnded - holdStarted : 0)},
            {"hold_started", Json(holdStarted != 0)},
            {"hold_joined_client_seconds", Json(mHoldJoinedExposureSeconds)},
            {"hold_actual_hz_per_joined_client", mHoldJoinedExposureSeconds > 0
                ? Json(static_cast<double>(holdStatesSent) / mHoldJoinedExposureSeconds) : Json()},
            {"hold_actual_periodic_hz_per_joined_client", mHoldJoinedExposureSeconds > 0
                ? Json(static_cast<double>(holdStatesSent - holdInitialStates) / mHoldJoinedExposureSeconds) : Json()},
            {"hold_state_batch_frames", Json(holdBatchFrames)},
            {"hold_state_batch_items_received", Json(holdBatchItems)},
            {"hold_state_updates_received", Json(holdStateUpdates)},
            {"hold_visibility_enters", Json(holdEnters)},
            {"hold_visible_edge_seconds", Json(mHoldVisibleExposureSeconds)},
            {"hold_actual_state_update_hz_per_visible_edge", mHoldVisibleExposureSeconds > 0
                ? Json(static_cast<double>(holdStateUpdates) / mHoldVisibleExposureSeconds) : Json()},
            {"joined_client_seconds", Json(mJoinedExposureSeconds)},
            {"visible_edge_seconds", Json(mVisibleExposureSeconds)},
            {"receive_frame_cap", Json(mOptions.maxFrame)}, {"elapsed_seconds", Json(Seconds(Clock::now()) - start)},
            {"counts", Json(std::move(counts))}, {"error_samples", Json(std::move(errors))},
            {"connect_latency", mConnectLatency.ToJson()}, {"join_latency", mJoinLatency.ToJson()},
            {"state_relay_latency", mRelayLatency.ToJson()},
            {"hold_state_update_latency", mHoldRelayLatency.ToJson()},
            {"final_visible_source_age", mFinalSourceAge.ToJson()},
            {"final_visible_receive_age", mFinalReceiveAge.ToJson()},
            {"server_idle_close_age", mIdleLatency.ToJson()},
            {"remaining_local_sockets", Json(mActive + mUdpActive)},
            {"remaining_local_tcp_sockets", Json(mActive)}, {"remaining_local_udp_sockets", Json(mUdpActive)},
            {"remaining_local_workers", Json(0)},
            {"notes", Json(std::string("Single-process nonblocking client; no server-side resource telemetry. "
                "Counts prove only this run, not the 65536 configuration ceiling. PlayerState is best-effort; "
                "frame counts are not an exact delivery guarantee. Hold begins after directory and negotiated UDP readiness; "
                "visibility may still be warming up. Update Hz counts newer StateBatch sequences, excluding Enter. "
                "Relay latency starts when a complete state is accepted by local TCP/UDP send, not when scheduled. "
                "Bytes include TCP framing or UDP 28-byte headers; UDP retransmissions/stale revisions do not inflate update Hz. "
                "Each UDP peer owns an additional local socket. No UDP advertisement means legacy TCP fallback. "
                "Unknown/expired source history is counted separately and excluded from latency percentiles. "
                "Final source age describes the last displayed state; no arbitrary latency limit is a PASS condition. "
                "TCP closure does not prove server reclamation."))}
        };
        const Json reportBody(std::move(report));
        const auto json = reportBody.Dump();
        if (!json.IsOk()) throw std::runtime_error("Could not serialize report");
        if (!mOptions.output.parent_path().empty()) std::filesystem::create_directories(mOptions.output.parent_path());
        std::ofstream file(mOptions.output, std::ios::binary | std::ios::trunc);
        file << json.Value() << '\n';
        file.close();
        if (!file) throw std::runtime_error("Could not write result file");
        std::cout << (passed ? "PASS" : cancelled ? "CANCELLED" : "FAIL") << " (connection/protocol checks): peak connected="
            << mCounts["peak_connected"] << ", peak joined=" << mCounts["peak_joined"]
            << ", states sent/received=" << mCounts["states_sent"] << '/' << mCounts["states_received"]
            << ", remaining local sockets=" << mActive + mUdpActive << ". Report: " << mOptions.output.string() << '\n';
        // Keeping connections alive does not prove that a game can meet its latency target.
        // Show the measured delays here too, so the interactive user need not open JSON first.
        for (const auto* name : { "connect_latency", "join_latency", "state_relay_latency",
            "hold_state_update_latency", "final_visible_source_age" })
        {
            const Json& latency = Field(reportBody, name);
            std::cout << "  " << name << " p95: ";
            if (const auto* value = Field(latency, "p95_ms").TryNumber())
                std::cout << std::fixed << std::setprecision(2) << *value << " ms\n";
            else
                std::cout << "n/a\n";
        }
        for (const auto* name : { "hold_actual_periodic_hz_per_joined_client",
            "hold_actual_state_update_hz_per_visible_edge" })
        {
            std::cout << "  " << name << ": ";
            if (const auto* value = Field(reportBody, name).TryNumber())
                std::cout << std::fixed << std::setprecision(2) << *value << " Hz\n";
            else std::cout << "n/a\n";
        }
        std::cout << "PASS covers connections and protocol checks; assess rate and latency separately.\n";
        return cancelled ? 130 : passed ? 0 : 1;
    }

private:
    void AccumulateExposure(const double now)
    {
        if (!mMeasuring) return;
        const double elapsed = (std::max)(0.0, now - mExposureUpdatedAt);
        mJoinedExposureSeconds += elapsed * static_cast<double>(mJoined);
        mVisibleExposureSeconds += elapsed * static_cast<double>(mVisibleEdges);
        if (mHoldStartedAt != 0)
        {
            mHoldJoinedExposureSeconds += elapsed * static_cast<double>(mJoined);
            mHoldVisibleExposureSeconds += elapsed * static_cast<double>(mVisibleEdges);
        }
        mExposureUpdatedAt = (std::max)(mExposureUpdatedAt, now);
    }

    void Error(const std::string& kind, const std::string& detail)
    {
        ++mCounts["errors"];
        ++mCounts[kind];
        if (mErrors.size() < MaximumErrorSamples) mErrors.push_back(kind + ": " + detail.substr(0, 200));
    }

    void Close(Connection& client, const std::string& reason)
    {
        if (client.socket == INVALID_SOCKET) return;
        AccumulateExposure(Seconds(Clock::now()));
        if (client.stage == Stage::Connecting) --mConnecting;
        else --mActive;
        if (client.stage == Stage::Joining) --mJoining;
        if (client.stage == Stage::Joined)
        {
            --mJoined;
            if (!client.directoryReady) --mSynchronizing;
            if (client.udpAdvertised)
            {
                if (client.udpReady) --mUdpReady;
                else --mUdpPending;
            }
        }
        if (client.udpSocket != INVALID_SOCKET)
        {
            closesocket(client.udpSocket);
            client.udpSocket = INVALID_SOCKET;
            --mUdpActive;
        }
        closesocket(client.socket);
        client.socket = INVALID_SOCKET;
        client.stage = Stage::Closed;
        client.send.clear();
        client.receive = Framer{};
        client.known.clear();
        mVisibleEdges -= client.visible.size();
        client.visible.clear();
        client.nextAck.clear();
        mAttempts[client.attempt].active = false;
        // Departed senders keep their identity, but not a per-state allocation.
        // Late in-flight observations are counted as missing history explicitly.
        std::vector<std::pair<std::uint64_t, double>>{}.swap(mAttempts[client.attempt].history);
        ++mCounts[reason];
    }

    void Fail(Connection& client, const std::string& kind, const std::string& detail)
    {
        Error(kind, detail);
        Close(client, "failed_local_closes");
    }

    void Open(Connection& client, const double now)
    {
        if (mAttempts.size() >= MaximumAttempts)
        {
            Error("attempt_limit", "Bounded attempt history exhausted; lower churn or duration");
            gStop.store(true, std::memory_order_relaxed);
            return;
        }
        client = Connection{};
        client.attempt = mAttempts.size();
        mAttempts.push_back(Attempt{ mRunPrefix + std::to_string(client.attempt), 0, 0,
            static_cast<unsigned int>(client.attempt % 5) });
        const auto position = Position(mOptions, static_cast<std::size_t>(&client - mClients.data()));
        mAttempts.back().x = position.first;
        mAttempts.back().y = position.second;
        ++mCounts["connect_attempted"];
        client.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client.socket == INVALID_SOCKET)
        {
            Error("socket_failed", std::to_string(WSAGetLastError()));
            return;
        }
        client.started = now;
        client.stage = Stage::Connecting;
        ++mConnecting;
        u_long nonblocking = 1;
        const BOOL noDelay = TRUE;
        if (ioctlsocket(client.socket, FIONBIO, &nonblocking) != 0 ||
            setsockopt(client.socket, IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char*>(&noDelay), static_cast<int>(sizeof(noDelay))) != 0)
        {
            Fail(client, "socket_config_failed", std::to_string(WSAGetLastError()));
            return;
        }
        const int result = connect(client.socket, reinterpret_cast<const sockaddr*>(&mAddress), static_cast<int>(sizeof(mAddress)));
        if (result == 0) Connected(client, now);
        else if (const int error = WSAGetLastError(); error != WSAEWOULDBLOCK && error != WSAEINPROGRESS)
            Fail(client, "connect_failed", std::to_string(error));
    }

    void Connected(Connection& client, const double now)
    {
        --mConnecting;
        ++mActive;
        ++mCounts["connect_succeeded"];
        mCounts["peak_connected"] = std::max(mCounts["peak_connected"], mActive);
        mConnectLatency.Add((now - client.started) * 1000.0);
        client.connected = now;
        client.stage = Stage::Connected;
        if (mOptions.scenario == "join" || mOptions.scenario == "churn")
        {
            const auto& attempt = mAttempts[client.attempt];
            client.stage = Stage::Joining;
            ++mJoining;
            Json::Object join{{"schemaVersion", Json(6)}, {"name", Json(attempt.name)}, {"c", Json(attempt.character)}};
            if (mOptions.movementTransport == "udp") join.emplace("movementTransport", Json(std::string("udp")));
            client.send = Encode("Join", Json(std::move(join)));
            client.sendStarted = now;
        }
    }

    void SendState(Connection& client, const double now)
    {
        ++mCounts["state_send_attempted"];
        if (client.udpAdvertised && !client.udpReady) { ++mCounts["udp_unready_state_skips"]; return; }
        if (!client.udpAdvertised && (!client.send.empty() || !client.nextAck.empty()))
            { ++mCounts["state_client_backpressure_skips"]; return; }
        auto& attempt = mAttempts[client.attempt];
        ++attempt.statesIssued;
        // Deliberately request c=5. Every ordinary load name must still relay its
        // approved A-E character, proving that movement cannot spoof a profile.
        Json state(Json::Object{
            {"x", Json(attempt.x)}, {"y", Json(attempt.y)}, {"q", Json(attempt.statesIssued)},
            {"vx", Json(0.0)}, {"vy", Json(0.0)}, {"f", Json(1.0)},
            {"s", Json(std::string("walk"))}, {"c", Json(5)} });
        if (client.udpAdvertised)
        {
            if (SendUdp(client, "PlayerState", state))
            {
                RecordStateSent(client);
                ++mCounts["udp_states_sent"];
            }
            return;
        }
        client.send = Encode("PlayerState", state);
        client.sent = 0;
        client.pendingState = true;
        client.sendStarted = now;
    }

    void RecordStateSent(Connection& client)
    {
        ++mCounts["states_sent"];
        auto& attempt = mAttempts[client.attempt];
        attempt.lastStateSentSerial = attempt.statesIssued;
        attempt.lastStateSentTime = Seconds(Clock::now());
        if (attempt.firstStateSentTime == 0)
        {
            attempt.firstStateSentTime = attempt.lastStateSentTime;
            ++mCounts["initial_states_sent"];
        }
        if (attempt.history.empty()) attempt.history.resize(StateHistorySize);
        attempt.history[attempt.statesIssued % StateHistorySize] =
            { attempt.statesIssued, attempt.lastStateSentTime };
    }

    bool SendUdp(Connection& client, const std::string_view type, const Json& body)
    {
        // Reuse the checked envelope encoder, but omit its TCP length prefix.
        const auto frame = Encode(type, body);
        std::array<std::byte, Datagram::MaximumDatagramBytes> bytes{};
        const auto size = Datagram::Encode(bytes, client.udpToken, ++client.udpSendSequence,
            std::span(frame).subspan(Codec::HeaderSize));
        if (size == 0) { Fail(client, "udp_encode_failed", "Movement exceeded the datagram budget"); return false; }
        const int count = send(client.udpSocket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(size), 0);
        if (count == static_cast<int>(size))
        {
            ++mCounts["udp_datagrams_sent"];
            mCounts["udp_bytes_sent"] += size;
            mCounts["bytes_sent"] += size;
            return true;
        }
        if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            ++mCounts["udp_send_would_block"];
            return false;
        }
        Fail(client, "udp_send_failed", std::to_string(WSAGetLastError()));
        return false;
    }

    void SendUdpHello(Connection& client, const double now)
    {
        client.nextUdpHello = now + 0.25;
        if (SendUdp(client, "UdpHello", Json(Json::Object{}))) ++mCounts["udp_hellos_sent"];
    }

    void NegotiateUdp(Connection& client, const Json& advertisement)
    {
        if (mOptions.movementTransport != "udp") throw std::runtime_error("Unrequested UDP advertisement");
        const auto port = Unsigned(Field(advertisement, "port"));
        const auto* tokenText = Field(advertisement, "token").TryString();
        const auto token = tokenText ? Datagram::TokenFromHex(*tokenText) : std::nullopt;
        if (port == 0 || port > 65535 || !token) throw std::runtime_error("Invalid UDP advertisement");
        client.udpAdvertised = true;
        ++mUdpPending;
        ++mCounts["udp_advertised_sessions"];
        client.udpToken = *token;
        client.udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (client.udpSocket == INVALID_SOCKET) throw std::runtime_error("UDP socket creation failed: " + std::to_string(WSAGetLastError()));
        ++mUdpActive;
        mCounts["peak_udp_sockets"] = std::max(mCounts["peak_udp_sockets"], mUdpActive);
        u_long nonblocking = 1;
        sockaddr_in address = mAddress;
        address.sin_port = htons(static_cast<u_short>(port));
        if (ioctlsocket(client.udpSocket, FIONBIO, &nonblocking) != 0 ||
            connect(client.udpSocket, reinterpret_cast<const sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0)
            throw std::runtime_error("UDP socket configuration failed: " + std::to_string(WSAGetLastError()));
        const double now = Seconds(Clock::now());
        client.nextHeartbeat = now + 5.0;
        SendUdpHello(client, now);
    }

    void Write(Connection& client, const bool cleaning = false)
    {
        if (client.send.empty() && !client.nextAck.empty())
        {
            client.send = std::move(client.nextAck);
            client.nextAck.clear();
            client.sent = 0;
            client.pendingAck = true;
            client.sendStarted = Seconds(Clock::now());
        }
        if (client.send.empty()) return;
        const int count = send(client.socket, reinterpret_cast<const char*>(client.send.data() + client.sent),
            static_cast<int>(client.send.size() - client.sent), 0);
        if (count > 0)
        {
            client.sent += static_cast<std::size_t>(count);
            mCounts["bytes_sent"] += static_cast<std::uint64_t>(count);
            if (client.sent == client.send.size())
            {
                if (client.pendingState) RecordStateSent(client);
                else ++mCounts[client.pendingAck ? "directory_acks_sent" : client.pendingHeartbeat ? "heartbeats_sent" : "joins_sent"];
                if (cleaning) ++mCounts["cleanup_frames_flushed"];
                client.send.clear();
                client.sent = 0;
                client.pendingState = false;
                client.pendingAck = false;
                client.pendingHeartbeat = false;
            }
        }
        else if (count == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
        {
            if (cleaning) Close(client, "cleanup_send_failed_closes");
            else Fail(client, "send_failed", std::to_string(WSAGetLastError()));
        }
    }

    static bool Knows(const Connection& client, const std::size_t index)
    {
        return index / 64 < client.known.size() && (client.known[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
    }

    static void Remember(Connection& client, const std::size_t index)
    {
        if (client.known.size() <= index / 64) client.known.resize(index / 64 + 1);
        client.known[index / 64] |= std::uint64_t{1} << (index % 64);
    }

    std::size_t Profile(const Json& profile)
    {
        const auto* name = Field(profile, "name").TryString();
        if (!name || !name->starts_with(mRunPrefix))
            throw std::runtime_error("Foreign profile: use an isolated test server");
        const auto ordinal = ParseUnsigned(std::string_view(*name).substr(mRunPrefix.size()));
        if (ordinal >= mAttempts.size()) throw std::runtime_error("Profile has an unrequested name");
        const auto index = static_cast<std::size_t>(ordinal);
        auto& attempt = mAttempts[index];
        if (attempt.name != *name || Unsigned(Field(profile, "c")) != attempt.character)
            throw std::runtime_error("Server profile name/character differs from requested profile");
        const auto id = SessionId(profile);
        if (attempt.id != 0 && attempt.id != id) throw std::runtime_error("One attempt received inconsistent IDs");
        const auto [position, inserted] = mIdToAttempt.emplace(id, index);
        if (!inserted && position->second != index) throw std::runtime_error("Server reused an ID for another attempt");
        attempt.id = id;
        return index;
    }

    void Handle(Connection& client, const std::span<const std::byte> bytes, const bool udp = false)
    {
        if (bytes.size() > 8u * 1024u) throw std::runtime_error("Schema 6 server frame exceeded 8 KiB");
        auto parsed = ServerCore::Protocol::ParseMessage(bytes);
        if (!parsed.IsOk()) throw std::runtime_error("Invalid UTF-8/JSON message envelope");
        const auto& message = parsed.Value();
        ++mCounts["frames_received"];
        mCounts["maximum_received_body"] = std::max(mCounts["maximum_received_body"], static_cast<std::uint64_t>(bytes.size()));
        const auto type = message.Type();
        if (udp && type != "UdpReady" && type != "StateBatch") throw std::runtime_error("Control message used UDP");
        if (type == "JoinRejected")
        {
            if (client.stage != Stage::Joining || !message.Error()) throw std::runtime_error("Unexpected JoinRejected");
            const auto* code = Field(*message.Error(), "code").TryString();
            if (!code) throw std::runtime_error("JoinRejected code is not a string");
            ++mCounts["join_rejected"];
            if (mErrors.size() < MaximumErrorSamples) mErrors.push_back("JoinRejected: " + code->substr(0, 100));
            // Even retryable rejections are failures for these unique, valid load profiles.
            throw JoinRejection("Join rejected: " + code->substr(0, 100));
        }
        const Json* body = message.Body();
        if (!body) throw std::runtime_error("Expected message body");
        if (type == "JoinAccepted")
        {
            if (client.stage != Stage::Joining || Unsigned(Field(*body, "schemaVersion")) != 6 ||
                Profile(*body) != client.attempt || Unsigned(Field(*body, "capacity")) == 0)
                throw std::runtime_error("Unexpected or invalid JoinAccepted");
            const auto* players = Field(*body, "players").TryArray();
            if (!players || !players->empty()) throw std::runtime_error("Schema 6 directory must be paged");
            const auto* through = Field(*body, "directoryThroughId").TryString();
            if (!through) throw std::runtime_error("Directory upper bound must be a decimal string");
            client.directoryThrough = ParseUnsigned(*through);
            for (const auto& profile : *players)
            {
                const auto index = Profile(profile);
                if (index == client.attempt || Knows(client, index)) throw std::runtime_error("Duplicate/self roster member");
                Remember(client, index);
            }
            AccumulateExposure(Seconds(Clock::now()));
            --mJoining;
            ++mJoined;
            ++mSynchronizing;
            ++mCounts["join_accepted"];
            mCounts["peak_joined"] = std::max(mCounts["peak_joined"], mJoined);
            mCounts["advertised_player_capacity"] = Unsigned(Field(*body, "capacity"));
            mJoinLatency.Add((Seconds(Clock::now()) - client.connected) * 1000.0);
            client.stage = Stage::Joined;
            // Schema 6 needs one initial position; this is independent of the
            // requested periodic rate, and is included in states_sent.
            if (const auto* advertisement = body->Find("udp")) NegotiateUdp(client, *advertisement);
            else
            {
                ++mCounts["tcp_movement_sessions"];
                if (mOptions.movementTransport == "udp") ++mCounts["udp_unavailable_tcp_fallback_sessions"];
                SendState(client, Seconds(Clock::now()));
            }
            return;
        }
        if (client.stage != Stage::Joined) throw std::runtime_error("Broadcast before JoinAccepted");
        if (type == "UdpReady")
        {
            if (!udp || !client.udpAdvertised || !body->TryObject()) throw std::runtime_error("Unexpected UdpReady");
            if (client.udpReady) { ++mCounts["duplicate_udp_ready"]; return; }
            client.udpReady = true;
            --mUdpPending;
            ++mUdpReady;
            ++mCounts["udp_ready_received"];
            SendState(client, Seconds(Clock::now()));
            return;
        }
        if (type == "DirectoryPage")
        {
            if (client.directoryReady || !client.nextAck.empty()) throw std::runtime_error("Unexpected directory page");
            const auto* cursorText = Field(*body, "cursor").TryString();
            if (!cursorText) throw std::runtime_error("Directory cursor is not a string");
            const auto cursor = ParseUnsigned(*cursorText);
            if (cursor <= client.directoryCursor || cursor > client.directoryThrough)
                throw std::runtime_error("Directory cursor did not advance within its upper bound");
            const auto* players = Field(*body, "players").TryArray();
            if (!players || players->empty() || players->size() > 32) throw std::runtime_error("Invalid directory page size");
            auto previous = client.directoryCursor;
            for (const auto& profile : *players)
            {
                const auto index = Profile(profile);
                const auto id = mAttempts[index].id;
                if (index == client.attempt || id <= previous || id > cursor)
                    throw std::runtime_error("Directory page is not an ordered non-self profile page");
                previous = id;
                Remember(client, index);
            }
            client.directoryCursor = cursor;
            client.nextAck = Encode("DirectoryAck", Json(Json::Object{ {"cursor", Json(*cursorText)} }));
            ++mCounts["directory_pages_received"];
            return;
        }
        if (type == "DirectoryReady")
        {
            if (client.directoryReady) throw std::runtime_error("Duplicate DirectoryReady");
            client.directoryReady = true;
            --mSynchronizing;
            ++mCounts["directory_ready_received"];
            return;
        }
        if (type == "ServerNotice")
        {
            const auto* kind = Field(*body, "kind").TryString();
            const auto* notice = Field(*body, "text").TryString();
            if (!kind || !notice || notice->empty() || notice->size() > 512 ||
                std::any_of(notice->begin(), notice->end(), [](const unsigned char c) { return c < 0x20 || c == 0x7f; }))
                throw std::runtime_error("Invalid server notice text");
            (void)Unsigned(Field(*body, "t"));
            if (*kind == "join" || *kind == "leave")
            {
                const auto* name = Field(*body, "name").TryString();
                if (!name || !name->starts_with(mRunPrefix)) throw std::runtime_error("Foreign presence notice");
                const auto ordinal = ParseUnsigned(std::string_view(*name).substr(mRunPrefix.size()));
                if (ordinal >= mAttempts.size() || *name != mAttempts[ordinal].name)
                    throw std::runtime_error("Presence notice name was never requested");
                const auto id = SessionId(*body);
                if (mAttempts[ordinal].id != 0 && mAttempts[ordinal].id != id)
                    throw std::runtime_error("Presence notice used inconsistent ID");
                ++mCounts[*kind == "join" ? "join_notices_received" : "leave_notices_received"];
            }
            else if (*kind == "announcement") ++mCounts["announcements_received"];
            else throw std::runtime_error("Unknown server notice kind");
            return;
        }
        if (type == "PlayerJoined")
        {
            const auto index = Profile(*body);
            if (index == client.attempt) throw std::runtime_error("Self PlayerJoined broadcast");
            if (Knows(client, index)) ++mCounts["duplicate_roster_events"];
            Remember(client, index);
            ++mCounts["player_joined_received"];
            return;
        }
        if (type == "VisibilityReady")
        {
            if (!client.directoryReady || client.visibilityReady)
                throw std::runtime_error("Unexpected VisibilityReady");
            client.visibilityReady = true;
            ++mCounts["visibility_ready_received"];
            return;
        }
        if (type == "VisibilityEnter" || type == "StateBatch")
        {
            if (!client.directoryReady && !udp) throw std::runtime_error("Visibility before directory completion");
            const bool entering = type == "VisibilityEnter";
            if (!entering && client.udpAdvertised && !udp) throw std::runtime_error("Negotiated movement used TCP");
            const auto* states = Field(*body, entering ? "players" : "states").TryArray();
            if (!states || states->empty() || states->size() > 32)
                throw std::runtime_error("Invalid AOI batch size");
            for (const auto& state : *states) HandleState(client, state, entering, udp);
            ++mCounts[entering ? "visibility_enter_frames" : "state_batch_frames"];
            return;
        }
        if (type == "VisibilityExit")
        {
            const auto* ids = Field(*body, "ids").TryArray();
            if (!ids || ids->empty() || ids->size() > 32) throw std::runtime_error("Invalid visibility exit batch");
            for (const auto& value : *ids)
            {
                const auto* idText = value.TryString();
                if (!idText) throw std::runtime_error("Exit ID is not a string");
                const auto found = mIdToAttempt.find(ParseUnsigned(*idText));
                if (found == mIdToAttempt.end() || !client.visible.contains(found->second))
                    throw std::runtime_error("Exit without prior visibility enter");
                AccumulateExposure(Seconds(Clock::now()));
                client.visible.erase(found->second);
                --mVisibleEdges;
                ++mCounts["visibility_exits_received"];
            }
            return;
        }
        const auto id = SessionId(*body);
        const auto found = mIdToAttempt.find(id);
        if (found == mIdToAttempt.end() || found->second == client.attempt ||
            (client.directoryReady && !Knows(client, found->second)))
            throw std::runtime_error("Broadcast ID is unknown, self, or already left");
        const auto index = found->second;
        if (type == "PlayerLeft")
        {
            // A real leave may precede the page that would have introduced this
            // old ID. It must not resurrect a profile or fail initialization.
            if (Knows(client, index)) client.known[index / 64] &= ~(std::uint64_t{1} << (index % 64));
            AccumulateExposure(Seconds(Clock::now()));
            mVisibleEdges -= client.visible.erase(index);
            ++mCounts["player_left_received"];
            return;
        }
        throw std::runtime_error("Unexpected message type: " + std::string(type));
    }

    void HandleState(Connection& client, const Json& body, const bool entering, const bool udp)
    {
        const auto id = SessionId(body);
        const auto found = mIdToAttempt.find(id);
        if (found != mIdToAttempt.end() && found->second == client.attempt)
            throw std::runtime_error("Server echoed the recipient's own movement");
        // UDP may race ahead of its reliable Enter, or arrive after Exit/Left.
        // Its authenticated payload cannot create membership or visibility.
        if (udp && (found == mIdToAttempt.end() || !Knows(client, found->second) ||
            !client.visible.contains(found->second))) { ++mCounts["udp_untracked_state_items"]; return; }
        if (found == mIdToAttempt.end() || found->second == client.attempt || !Knows(client, found->second))
            throw std::runtime_error("AOI state ID is unknown, self, or left");
        const auto index = found->second;
        const auto revision = StateRevision(body, client.udpAdvertised);
        if (udp)
        {
            ++mCounts["udp_state_items_received"];
            if (revision <= client.visible.at(index).serverRevision)
            { ++mCounts["udp_stale_or_refresh_state_items"]; return; }
        }
        const auto& sender = mAttempts[index];
        const auto number = [&](const char* key) {
            const auto* value = Field(body, key).TryNumber();
            if (!value || !std::isfinite(*value)) throw std::runtime_error("Invalid state numeric field");
            return *value;
        };
        const auto serial = Unsigned(Field(body, "q"));
        const auto* state = Field(body, "s").TryString();
        if (number("x") != sender.x || number("y") != sender.y || serial < 1 ||
            serial > sender.statesIssued || number("vx") != 0 || number("vy") != 0 ||
            number("f") != 1 || !state || *state != "walk" || Unsigned(Field(body, "c")) != sender.character)
            throw std::runtime_error("PlayerState payload does not match its server-assigned sender ID");
        const auto& viewer = mAttempts[client.attempt];
        if (std::abs(viewer.x - sender.x) > (entering ? 32.0 : 36.0) ||
            std::abs(viewer.y - sender.y) > (entering ? 20.0 : 24.0))
            throw std::runtime_error("AOI sent a fixed-position state outside its interest rectangle");
        const auto timestamp = Unsigned(Field(body, "t"));
        const double receivedAt = Seconds(Clock::now());
        const double sourceSentAt = HistoryTime(sender, serial);
        bool newerState = false;
        if (entering)
        {
            const auto* name = Field(body, "name").TryString();
            if (!name || *name != sender.name || client.visible.contains(index))
                throw std::runtime_error("Duplicate enter or incorrect visibility profile");
            AccumulateExposure(receivedAt);
            client.visible.emplace(index, VisibleState{serial, timestamp, serial, receivedAt, receivedAt, sourceSentAt, false, revision});
            ++mVisibleEdges;
            ++mCounts["visibility_enters_received"];
        }
        else
        {
            const auto visible = client.visible.find(index);
            if (visible == client.visible.end() || serial < visible->second.sequence || timestamp < visible->second.serverTime)
                throw std::runtime_error("State arrived without Enter or moved backwards");
            ++mCounts["state_batch_items_received"];
            if (serial > visible->second.sequence)
            {
                ++mCounts["state_updates_received"];
                visible->second.receivedUpdate = true;
                newerState = true;
            }
            else ++mCounts["duplicate_state_items_received"];
            visible->second.sequence = serial;
            visible->second.serverTime = timestamp;
            visible->second.lastReceiveAt = receivedAt;
            visible->second.sourceSendTime = sourceSentAt;
            visible->second.serverRevision = revision;
        }
        ++mCounts["states_received"];
        if (sourceSentAt != 0)
        {
            mRelayLatency.Add((receivedAt - sourceSentAt) * 1000.0);
            if (newerState && mMeasuring && mHoldStartedAt != 0)
                mHoldRelayLatency.Add((receivedAt - sourceSentAt) * 1000.0);
        }
        else
        {
            ++mCounts["relay_history_expired"];
            if (!sender.active) ++mCounts["relay_history_missing_closed_sender"];
        }
    }

    void CheckFinalVisibility(const bool checkGeometry, const double now)
    {
        for (const auto& client : mClients)
        {
            if (client.stage != Stage::Joined) continue;
            const auto& viewer = mAttempts[client.attempt];
            if (checkGeometry && (!client.directoryReady || !client.visibilityReady))
            {
                Error("visibility_incomplete", "Joined peer did not complete directory and initial visibility");
                continue;
            }
            mCounts["final_visible_edges"] += client.visible.size();
            mCounts["maximum_visible_per_client"] = std::max(mCounts["maximum_visible_per_client"],
                static_cast<std::uint64_t>(client.visible.size()));
            for (const auto& [index, visible] : client.visible)
            {
                const auto& sender = mAttempts[index];
                // Churn can leave a valid final in-flight state until PlayerLeft arrives.
                if (!sender.active) { ++mCounts["final_visible_departed_in_flight"]; continue; }
                mFinalReceiveAge.Add((now - visible.lastReceiveAt) * 1000.0);
                if (visible.sourceSendTime != 0)
                    mFinalSourceAge.Add((now - visible.sourceSendTime) * 1000.0);
                else ++mCounts["final_visible_source_age_history_missing"];
                if (!visible.receivedUpdate) ++mCounts["final_visible_never_updated_edges"];
                if (MissingStateUpdates(sender, visible, now))
                    Error("missing_state_updates", "Visible peer never received a newer state despite later source sends");
            }
            if (!checkGeometry) continue;
            // Compare fixed-position peers after a bounded settling allowance;
            // omitted nearby peers are a failure, not silently a lower load.
            for (std::size_t index = 0; index < mAttempts.size(); ++index)
            {
                const auto& sender = mAttempts[index];
                if (index == client.attempt || !sender.active || sender.firstStateSentTime == 0 ||
                    now - sender.firstStateSentTime < 0.5) continue;
                const bool expected = std::abs(viewer.x - sender.x) <= 32.0 && std::abs(viewer.y - sender.y) <= 20.0;
                if (expected != client.visible.contains(index))
                    Error("visibility_mismatch", "Final AOI membership differs from fixed-position geometry");
                if (!Knows(client, index)) Error("directory_missing", "Completed global directory omitted an active peer");
            }
        }
    }

    void Read(Connection& client, const bool cleaning)
    {
        std::array<std::byte, 8192> buffer;
        for (unsigned int batch = 0; batch < 8 && client.socket != INVALID_SOCKET; ++batch)
        {
            const int count = recv(client.socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (count > 0)
            {
                mCounts["bytes_received"] += static_cast<std::uint64_t>(count);
                if (cleaning) { mCounts["cleanup_bytes_drained"] += static_cast<std::uint64_t>(count); continue; }
                try
                {
                    client.receive.Feed(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count)),
                        mOptions.maxFrame, [&](const auto body) { Handle(client, body); });
                }
                catch (const JoinRejection&) { Close(client, "rejected_local_closes"); }
                catch (const std::exception& error) { Fail(client, "protocol_errors", error.what()); }
                continue;
            }
            if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return;
            const double age = Seconds(Clock::now()) - client.connected;
            const int socketError = count < 0 ? WSAGetLastError() : 0;
            if (cleaning) Close(client, "cleanup_peer_closes");
            else if ((count == 0 || socketError == WSAECONNRESET) && mOptions.scenario == "idle" &&
                age + 0.25 >= mOptions.idleTimeout && !client.receive.Incomplete())
            {
                mIdleLatency.Add(age * 1000.0);
                Close(client, "expected_idle_closes");
            }
            else
            {
                if (client.receive.Incomplete()) Error("truncated_frames", "Peer ended a partial frame");
                if (mErrors.size() < MaximumErrorSamples)
                    mErrors.push_back("Unexpected close at age " + std::to_string(age) + " seconds; WSA=" +
                        std::to_string(socketError));
                Close(client, "unexpected_disconnects");
            }
            return;
        }
    }

    void ReadUdp(Connection& client, const bool cleaning)
    {
        std::array<std::byte, Datagram::MaximumDatagramBytes + 1> bytes{};
        for (unsigned int batch = 0; batch < 16 && client.udpSocket != INVALID_SOCKET; ++batch)
        {
            const int count = recv(client.udpSocket, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
            if (count == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK) return;
                if (error == WSAEMSGSIZE) { ++mCounts["udp_invalid_datagrams"]; continue; }
                if (cleaning) { ++mCounts["cleanup_udp_receive_errors"]; return; }
                Fail(client, "udp_receive_failed", std::to_string(error));
                return;
            }
            mCounts["bytes_received"] += static_cast<std::uint64_t>(count);
            mCounts["udp_bytes_received"] += static_cast<std::uint64_t>(count);
            ++mCounts["udp_datagrams_received"];
            if (cleaning) { mCounts["cleanup_bytes_drained"] += static_cast<std::uint64_t>(count); continue; }
            const auto packet = Datagram::Decode(std::span(bytes).first(static_cast<std::size_t>(count)));
            if (!packet || packet->token != client.udpToken) { ++mCounts["udp_invalid_datagrams"]; continue; }
            // Different packets can contain disjoint entities: a packet sequence
            // inversion is observable, but only each entity's r decides staleness.
            if (packet->sequence <= client.udpLargestReceivedSequence) ++mCounts["udp_reordered_or_duplicate_packets"];
            client.udpLargestReceivedSequence = std::max(client.udpLargestReceivedSequence, packet->sequence);
            try { Handle(client, packet->payload, true); }
            catch (const std::exception& error) { Fail(client, "protocol_errors", error.what()); return; }
        }
    }

    void Poll(const bool cleaning = false, const bool flushing = false)
    {
        mPoll.clear();
        mPollSlots.clear();
        for (std::size_t index = 0; index < mClients.size(); ++index)
        {
            const auto& client = mClients[index];
            if (client.socket == INVALID_SOCKET) continue;
            const short events = static_cast<short>(POLLRDNORM |
                (((!cleaning || flushing) && (client.stage == Stage::Connecting || !client.send.empty() ||
                    !client.nextAck.empty())) ? POLLWRNORM : 0));
            mPoll.push_back(WSAPOLLFD{ client.socket, events, 0 });
            mPollSlots.emplace_back(index, false);
            if (client.udpSocket != INVALID_SOCKET)
            {
                mPoll.push_back(WSAPOLLFD{client.udpSocket, POLLRDNORM, 0});
                mPollSlots.emplace_back(index, true);
            }
        }
        if (mPoll.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); return; }
        const int ready = WSAPoll(mPoll.data(), static_cast<ULONG>(mPoll.size()), 2);
        if (ready < 0) throw std::runtime_error("WSAPoll failed: " + std::to_string(WSAGetLastError()));
        for (std::size_t index = 0; index < mPoll.size(); ++index)
        {
            if (index % 32 == 0 && (Seconds(Clock::now()) >= (cleaning ? mCleanupDeadline : mWorkDeadline) ||
                (!cleaning && gStop.load(std::memory_order_relaxed)))) break;
            auto& client = mClients[mPollSlots[index].first];
            const short events = mPoll[index].revents;
            if (events == 0 || client.socket == INVALID_SOCKET) continue;
            if (mPollSlots[index].second)
            {
                if (client.udpSocket != INVALID_SOCKET) ReadUdp(client, cleaning);
                continue;
            }
            if (client.stage == Stage::Connecting)
            {
                int error = 0;
                int length = static_cast<int>(sizeof(error));
                if (getsockopt(client.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 ||
                    error != 0 || (events & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                {
                    Fail(client, "connect_failed", std::to_string(error != 0 ? error : WSAGetLastError()));
                    continue;
                }
                if ((events & POLLWRNORM) != 0) Connected(client, Seconds(Clock::now()));
                else continue;
            }
            if ((!cleaning || flushing) && (events & POLLWRNORM) != 0) Write(client, cleaning);
            if (client.socket != INVALID_SOCKET && (events & (POLLRDNORM | POLLERR | POLLHUP | POLLNVAL)) != 0)
                Read(client, cleaning);
        }
    }

    void Cleanup()
    {
        // Finish a partially sent frame before FIN. A bounded failure to flush is
        // reported separately, rather than silently truncating a valid load frame.
        const double start = Seconds(Clock::now());
        mCleanupDeadline = start + 3.0;
        for (auto& client : mClients)
            if (client.stage == Stage::Connecting) Close(client, "cleanup_connecting_closes");
        try
        {
            while (Seconds(Clock::now()) < start + 1.0 && std::any_of(mClients.begin(), mClients.end(),
                [](const Connection& client) { return !client.send.empty(); })) Poll(true, true);
        }
        catch (const std::exception& error) { Error("cleanup_flush_error", error.what()); }
        for (auto& client : mClients)
        {
            if (!client.send.empty()) Close(client, "cleanup_forced_partial_closes");
            else if (client.socket != INVALID_SOCKET)
            {
                (void)shutdown(client.socket, SD_SEND);
            }
        }
        try
        {
            while (mActive != 0 && Seconds(Clock::now()) < mCleanupDeadline) Poll(true);
        }
        catch (const std::exception& error) { Error("cleanup_poll_error", error.what()); }
        for (auto& client : mClients) Close(client, "cleanup_forced_local_closes");
    }

    Options mOptions;
    sockaddr_in mAddress{};
    std::string mRunPrefix;
    std::vector<Connection> mClients;
    std::vector<Attempt> mAttempts;
    std::unordered_map<std::uint64_t, std::size_t> mIdToAttempt;
    std::vector<std::size_t> mOrder;
    std::mt19937_64 mRandom;
    std::vector<WSAPOLLFD> mPoll;
    std::vector<std::pair<std::size_t, bool>> mPollSlots;
    std::unordered_map<std::string, std::uint64_t> mCounts;
    std::vector<std::string> mErrors;
    std::size_t mChurnCursor = 0;
    double mWorkDeadline = 0;
    double mCleanupDeadline = 0;
    std::uint64_t mActive = 0;
    std::uint64_t mConnecting = 0;
    std::uint64_t mJoining = 0;
    std::uint64_t mJoined = 0;
    std::uint64_t mSynchronizing = 0;
    std::uint64_t mUdpActive = 0;
    std::uint64_t mUdpReady = 0;
    std::uint64_t mUdpPending = 0;
    std::size_t mVisibleEdges = 0;
    double mExposureUpdatedAt = 0;
    double mHoldStartedAt = 0;
    double mJoinedExposureSeconds = 0;
    double mVisibleExposureSeconds = 0;
    double mHoldJoinedExposureSeconds = 0;
    double mHoldVisibleExposureSeconds = 0;
    bool mMeasuring = false;
    Latencies mConnectLatency;
    Latencies mJoinLatency;
    Latencies mIdleLatency;
    Latencies mRelayLatency;
    Latencies mHoldRelayLatency;
    Latencies mFinalSourceAge;
    Latencies mFinalReceiveAge;
};

void SelfCheck()
{
    const auto require = [](const bool condition) { if (!condition) throw std::runtime_error("Self-check failed"); };
    const auto frame = Encode("Join", Json(Json::Object{ {"schemaVersion", Json(6)},
        {"name", Json(std::string("load-check"))}, {"c", Json(2)} }));
    Framer reader;
    std::size_t calls = 0;
    const auto receive = [&](const std::span<const std::byte> body) {
        const auto message = ServerCore::Protocol::ParseMessage(body);
        require(message.IsOk() && message.Value().Type() == "Join");
        require(Unsigned(Field(*message.Value().Body(), "schemaVersion")) == 6);
        ++calls;
    };
    for (const auto byte : frame) reader.Feed(std::span<const std::byte>(&byte, 1), 65536, receive);
    require(calls == 1 && !reader.Incomplete());
    std::vector<std::byte> joined = frame;
    joined.insert(joined.end(), frame.begin(), frame.end());
    reader.Feed(joined, 65536, receive);
    require(calls == 3 && !reader.Incomplete());
    reader.Feed(std::span<const std::byte>(frame).first(5), 65536, receive);
    require(reader.Incomplete());
    const auto rejects = [&](const std::array<std::byte, 4>& prefix) {
        Framer invalid;
        try { invalid.Feed(prefix, 65536, receive); } catch (const std::runtime_error&) { return true; }
        return false;
    };
    require(rejects({ std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0} }));
    require(rejects({ std::byte{1}, std::byte{0}, std::byte{1}, std::byte{0} }));
    const std::string invalid = "{\"type\":\"Bad\",\"body\":{\"x\":NaN}}";
    require(!ServerCore::Protocol::ParseMessage(std::as_bytes(std::span(invalid))).IsOk());
    Latencies latency(1);
    for (int value = 1; value <= 100; ++value) latency.Add(static_cast<double>(value));
    const auto summary = latency.ToJson();
    Attempt sender;
    sender.lastStateSentSerial = 2;
    sender.history.resize(StateHistorySize);
    sender.history[1] = {1, 10.0};
    sender.history[2] = {2, 11.0};
    VisibleState visible;
    visible.initialSequence = 1;
    visible.enteredAt = 10.0;
    visible.sequence = 1;
    visible.sourceSendTime = HistoryTime(sender, 1);
    require(visible.sourceSendTime == 10.0 && HistoryTime(sender, 257) == 0);
    require(!MissingStateUpdates(sender, visible, 11.999));
    require(MissingStateUpdates(sender, visible, 12.0));
    visible.receivedUpdate = true;
    require(!MissingStateUpdates(sender, visible, 12.0));
    visible.receivedUpdate = false;
    sender.active = false;
    require(!MissingStateUpdates(sender, visible, 12.0));
    sender.active = true;
    sender.lastStateSentSerial = 300;
    sender.history[45] = {45, 20.0};
    require(!MissingStateUpdates(sender, visible, 20.999));
    require(MissingStateUpdates(sender, visible, 21.0));
    sender.history[45] = {301, 20.0};
    require(!MissingStateUpdates(sender, visible, 21.0));
    // The oldest ordinal was not successfully sent, but another retained newer
    // state was: UDP WouldBlock must not hide a receiver that never updates.
    sender.history[46] = {46, 20.0};
    require(!MissingStateUpdates(sender, visible, 20.999));
    require(MissingStateUpdates(sender, visible, 21.0));
    sender.history[46] = {302, 20.0};
    require(!MissingStateUpdates(sender, visible, 21.0));
    // A captured source timestamp remains usable after the sender's finite ring wraps.
    require(visible.sourceSendTime == 10.0);
    require(*Field(summary, "p50_ms").TryNumber() == 50.0 && *Field(summary, "p99_ms").TryNumber() == 99.0);
    require(SessionId(Json(Json::Object{ {"id", Json(std::string("9007199254740993"))} })) == 9007199254740993ULL);
    require(StateRevision(Json(Json::Object{{"r", Json(std::string("18446744073709551615"))}}), true) == UINT64_MAX);
    for (const auto& badRevision : {Json(), Json(1), Json(std::string("01")), Json(std::string("18446744073709551616"))})
    {
        bool rejected = false;
        try { (void)StateRevision(Json(Json::Object{{"r", badRevision}}), true); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected);
    }
    const auto token = Datagram::TokenFromHex("00112233445566778899aabbccddeeff");
    require(token.has_value() && !Datagram::TokenFromHex("invalid").has_value());
    std::array<std::byte, Datagram::MaximumDatagramBytes> datagram{};
    const auto payload = std::span(frame).subspan(Codec::HeaderSize);
    const auto length = Datagram::Encode(datagram, *token, UINT64_MAX, payload);
    const auto decoded = Datagram::Decode(std::span(datagram).first(length));
    require(decoded && decoded->token == *token && decoded->sequence == UINT64_MAX &&
        std::equal(decoded->payload.begin(), decoded->payload.end(), payload.begin(), payload.end()));
    require(Datagram::Encode(datagram, *token, 0, payload) == 0);
    datagram[0] = std::byte{0};
    require(!Datagram::Decode(std::span(datagram).first(length)));
    std::cout << "Self-check passed: fragmented/coalesced frames, EOF state, length caps, invalid JSON, quantiles, exact uint64 IDs/revisions, UDP framing. No sockets opened.\n";
}
}

int main(const int argc, char* argv[])
{
    (void)SetConsoleOutputCP(CP_UTF8);
    (void)SetConsoleCP(CP_UTF8);
    try
    {
        const Options options = ParseOptions(argc, argv);
        if (options.help) { PrintHelp(); return 0; }
        if (options.selfCheck) { SelfCheck(); return 0; }
        WSADATA data{};
        if (const int error = WSAStartup(MAKEWORD(2, 2), &data); error != 0)
            throw std::runtime_error("WSAStartup failed: " + std::to_string(error));
        struct WinsockCleanup { ~WinsockCleanup() { WSACleanup(); } } winsockCleanup;
        if (!SetConsoleCtrlHandler(ConsoleControl, TRUE)) throw std::runtime_error("Could not install Ctrl+C handler");
        struct ControlCleanup { ~ControlCleanup() { SetConsoleCtrlHandler(ConsoleControl, FALSE); } } controlCleanup;
        LoadRun run(options);
        return run.Run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "SummitLoadTest: " << error.what() << '\n';
        return 2;
    }
}
