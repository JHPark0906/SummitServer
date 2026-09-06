#include "SummitServerBackend.h"
#include "SummitUdpTransport.h"
#include "ServerConsole.h"

#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Clock.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Dispatch/Dispatcher.h"
#include "ServerCore/Protocol/Json.h"
#include "ServerCore/Runtime/PeriodicRunner.h"
#include "ServerCore/Runtime/ServerHost.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace
{
// 로컬의 모든 IPv4 인터페이스에서 받는다. 라우터가 전달하는 공인 주소를 직접 bind하지 않는다.
constexpr std::string_view DefaultAddress = "0.0.0.0";
constexpr std::uint16_t DefaultPort = 17890;

// 콘솔 제어 콜백은 main과 다른 스레드에서 올 수 있다. 로드한 shared_ptr가 Stop 호출 중
// Host를 살려 두며, main은 종료 시 새 콜백이 소유권을 얻지 못하도록 이 슬롯을 비운다.
std::atomic<std::shared_ptr<ServerCore::Runtime::ServerHost>> gRunningHost{ nullptr };

class ConsoleLogger final : public ServerCore::Core::ILogger
{
public:
    void Write(const ServerCore::Core::LogLevel level, const std::string_view message) noexcept override
    {
        // I/O and JobRunner threads share stderr. Keep each record together,
        // and never propagate a logging failure into a network callback.
        try
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            std::fprintf(stderr, "[%s] ", LevelName(level));
            if (!message.empty())
            {
                std::fwrite(message.data(), 1, message.size(), stderr);
            }
            std::fputc('\n', stderr);
            std::fflush(stderr);
        }
        catch (...)
        {
        }
    }

private:
    static const char* LevelName(const ServerCore::Core::LogLevel level) noexcept
    {
        switch (level)
        {
        case ServerCore::Core::LogLevel::Trace: return "TRACE";
        case ServerCore::Core::LogLevel::Debug: return "DEBUG";
        case ServerCore::Core::LogLevel::Info: return "INFO";
        case ServerCore::Core::LogLevel::Warn: return "WARN";
        case ServerCore::Core::LogLevel::Error: return "ERROR";
        }
        return "UNKNOWN";
    }

    std::mutex mMutex;
};

struct CommandLineOptions
{
    std::string address{ DefaultAddress };
    std::uint16_t port = DefaultPort;
    std::uint32_t maxPlayers = static_cast<std::uint32_t>(Summit::SummitServerBackend::PlayerCapacity);
    std::uint32_t maxConnections = static_cast<std::uint32_t>(Summit::SummitServerBackend::ConnectionCapacity);
    std::uint32_t metricsIntervalMilliseconds = 0;
    Summit::SummitServerBackend::SchedulerOptions scheduler;
    bool showHelp = false;
};

void PrintUsage()
{
    std::cout << "Usage: SummitServer [--address <IPv4>] [--port <1-65535>]\n"
              << "                    [--max-sessions <1-65536>] [--metrics-interval-ms <0-3600000>]\n"
              << "                    [--state-bytes-per-tick <bytes>] [--total-state-bytes-per-tick <bytes>]\n"
              << "                    [--control-bytes-per-tick <bytes>] [--total-control-bytes-per-tick <bytes>]\n"
              << "Default listener: " << DefaultAddress << ':' << DefaultPort << '\n'
              << "Default capacity: 16 joined players / 64 connections.\n"
              << "--max-sessions sets both capacities; metrics interval 0 disables reporting.\n"
              << "Tick budgets: state 2048/client, 1048576/total; directory/AOI control 16384/client, 262144/total.\n"
              << "Each budget accepts 1024..16777216 bytes including TCP(4) or UDP(28) application headers.\n"
              << "Movement UDP uses the same port as TCP; allow/forward both protocols. Datagrams are at most 1200 bytes.\n"
              << "Console: /announce <message> sends a notice; /players lists joined players; /help shows commands.\n"
              << "Messages use 1..512 UTF-8 bytes. Redirected stdin must be UTF-8; EOF keeps the server running.\n";
}

bool ParseUnsigned(const std::string_view text, const std::uint32_t minimum,
    const std::uint32_t maximum, std::uint32_t& output) noexcept
{
    std::uint32_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < minimum || value > maximum)
    {
        return false;
    }

    output = value;
    return true;
}

bool ParseCommandLine(const int argc, char* argv[], CommandLineOptions& output)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h")
        {
            output.showHelp = true;
            continue;
        }
        if (argument == "--address")
        {
            if (++index >= argc)
            {
                std::cerr << "--address requires an IPv4 address.\n";
                return false;
            }
            output.address = argv[index];
            continue;
        }
        if (argument == "--port")
        {
            std::uint32_t port = 0;
            if (++index >= argc || !ParseUnsigned(argv[index], 1, 65535, port))
            {
                std::cerr << "--port requires an integer from 1 through 65535.\n";
                return false;
            }
            output.port = static_cast<std::uint16_t>(port);
            continue;
        }
        if (argument == "--max-sessions")
        {
            std::uint32_t capacity = 0;
            if (++index >= argc || !ParseUnsigned(argv[index], 1,
                static_cast<std::uint32_t>(Summit::SummitServerBackend::MaximumSessionCapacity), capacity))
            {
                std::cerr << "--max-sessions requires an integer from 1 through 65536.\n";
                return false;
            }
            output.maxPlayers = capacity;
            output.maxConnections = capacity;
            continue;
        }
        if (argument == "--metrics-interval-ms")
        {
            if (++index >= argc ||
                !ParseUnsigned(argv[index], 0, 3'600'000, output.metricsIntervalMilliseconds))
            {
                std::cerr << "--metrics-interval-ms requires an integer from 0 through 3600000.\n";
                return false;
            }
            continue;
        }

        // 예산은 네트워크가 받는 프레임 바이트다. 0/무제한으로 보호 경계를 끄지 않는다.
        std::size_t* budget = nullptr;
        if (argument == "--state-bytes-per-tick") budget = &output.scheduler.perClientStateBytesPerTick;
        else if (argument == "--total-state-bytes-per-tick") budget = &output.scheduler.globalStateBytesPerTick;
        else if (argument == "--control-bytes-per-tick") budget = &output.scheduler.perClientControlBytesPerTick;
        else if (argument == "--total-control-bytes-per-tick") budget = &output.scheduler.globalControlBytesPerTick;
        if (budget)
        {
            std::uint32_t value = 0;
            if (++index >= argc || !ParseUnsigned(argv[index], 1024, 16u * 1024u * 1024u, value))
            {
                std::cerr << argument << " requires an integer from 1024 through 16777216.\n";
                return false;
            }
            *budget = value;
            continue;
        }

        std::cerr << "Unknown argument: " << argument << '\n';
        return false;
    }
    return true;
}

void PrintFailure(const std::string_view operation, const ServerCore::Core::Status& status)
{
    std::cerr << operation << " failed (code " << static_cast<int>(status.Code())
              << "): " << status.Message() << '\n';
}

void PrintPlayers(Summit::SummitServerBackend& backend) noexcept
{
    try
    {
        auto snapshot = backend.SnapshotPlayers();
        if (!snapshot.IsOk())
        {
            PrintFailure("Players snapshot", snapshot.GetStatus());
            return;
        }
        using ServerCore::Protocol::JsonValue;
        constexpr std::array<std::string_view, 6> characterNames{ "A", "B", "C", "D", "E", "tdw" };
        const auto& players = snapshot.Value();
        std::string output = "Players: " + std::to_string(players.size()) +
            (players.empty() ? " (no joined players)\n" : " (joined)\n");
        for (const auto& player : players)
        {
            JsonValue::Object fields;
            fields.emplace("id", JsonValue(std::to_string(static_cast<std::uint64_t>(player.id))));
            fields.emplace("name", JsonValue(player.name));
            fields.emplace("character", JsonValue(player.character));
            fields.emplace("characterName", JsonValue(std::string(player.character < characterNames.size()
                ? characterNames[static_cast<std::size_t>(player.character)] : "unknown")));
            auto row = JsonValue(std::move(fields)).Dump();
            if (!row.IsOk())
            {
                PrintFailure("Players serialization", row.GetStatus());
                return;
            }
            output += row.Value();
            output.push_back('\n');
        }
        // 내부 명단의 Operation은 이미 끝났다. 모든 JSON 행을 준비한 뒤 한 번에 출력해
        // 실패한 조회의 일부나 다른 콘솔 스레드의 /help가 목록 중간에 끼지 않게 한다.
        if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size() || std::fflush(stdout) != 0)
            std::fputs("Players output failed.\n", stderr);
    }
    catch (...)
    {
        std::fputs("Players rejected: could not prepare the player list.\n", stderr);
    }
}

struct ConsoleCommandReservation
{
    explicit ConsoleCommandReservation(std::shared_ptr<std::atomic<std::uint32_t>> pending)
        : pendingCount(std::move(pending)) {}
    ~ConsoleCommandReservation() { pendingCount->fetch_sub(1, std::memory_order_relaxed); }
    std::shared_ptr<std::atomic<std::uint32_t>> pendingCount;
};

Summit::ServerConsole::LineHandler MakeConsoleHandler(
    ServerCore::Runtime::JobRunner::Lease runner, std::weak_ptr<Summit::SummitServerBackend> backend)
{
    const auto pending = std::make_shared<std::atomic<std::uint32_t>>(0);
    return [runner = std::move(runner), backend = std::move(backend), pending](std::string line) {
        if (line.empty()) return;
        if (line == "/help")
        {
            std::fputs("Console: /announce <message> (1..512 UTF-8 bytes); /players lists joined IDs, names and characters; /help shows commands; Ctrl+C stops the server.\n", stdout);
            std::fflush(stdout);
            return;
        }
        const bool listPlayers = line == "/players";
        if (!listPlayers && line != "/announce" && !line.starts_with("/announce ") && !line.starts_with("/announce\t"))
        {
            std::fputs("Unknown console command. Use /announce <message>, /players or /help.\n", stderr);
            return;
        }
        if (!listPlayers) line.erase(0, line.size() == 9 ? 9 : 10);
        // 리디렉션된 대량 입력도 JobRunner의 무제한 일반 작업 큐를 채우지 않게 제한한다.
        std::uint32_t count = pending->load(std::memory_order_relaxed);
        while (count < 64 && !pending->compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {}
        if (count >= 64)
        {
            std::fputs("Console command rejected: console queue is busy; retry later.\n", stderr);
            return;
        }
        std::shared_ptr<ConsoleCommandReservation> reservation;
        try
        {
            reservation = std::make_shared<ConsoleCommandReservation>(pending);
            const auto posted = runner.Post([backend, text = std::move(line), reservation, listPlayers]() {
                if (const auto current = backend.lock())
                {
                    if (listPlayers)
                    {
                        PrintPlayers(*current);
                        return;
                    }
                    const auto announced = current->Announce(text);
                    if (!announced.IsOk()) PrintFailure("Announcement", announced);
                    else
                    {
                        std::fputs("Announcement accepted for broadcast.\n", stdout);
                        std::fflush(stdout);
                    }
                }
            });
            if (!posted.IsOk()) PrintFailure("Console command scheduling", posted);
        }
        catch (...)
        {
            if (!reservation) pending->fetch_sub(1, std::memory_order_relaxed);
            std::fputs("Console command rejected: could not allocate a console command.\n", stderr);
        }
    };
}

struct MetricsSample
{
    std::uint64_t elapsedMilliseconds = 0;
    std::size_t activeSessions = 0;
    std::uint64_t receivedFrames = 0;
    std::uint64_t queuedSendFrames = 0;
    std::uint64_t errors = 0;
    std::size_t pendingReceiveBytes = 0;
    std::size_t pendingJobs = 0;
    std::size_t pendingParseBytes = 0;
    std::size_t pendingParseTasks = 0;
    std::size_t queuedSendBytes = 0;
    Summit::SummitServerBackend::AoiMetrics aoi;
    Summit::SummitUdpTransport::Metrics udp;

    void Print(const char* kind) const noexcept
    {
        std::printf("{\"kind\":\"%s\",\"elapsedMs\":%llu,\"activeSessions\":%zu,"
            "\"receivedFrames\":%llu,\"queuedSendFrames\":%llu,\"errors\":%llu,"
            "\"pendingReceiveBytes\":%zu,\"pendingJobs\":%zu,\"pendingParseBytes\":%zu,"
            "\"pendingParseTasks\":%zu,\"queuedSendBytes\":%zu,\"joinedPlayers\":%llu,"
            "\"positionedPlayers\":%llu,\"visibleEdges\":%llu,\"coalescedStates\":%llu,"
            "\"sentStateItems\":%llu,\"sentStateFrames\":%llu,\"tickCount\":%llu,"
            "\"sentStateBytes\":%llu,\"sentControlBytes\":%llu,"
            "\"lastTickStateBytes\":%llu,\"lastTickControlBytes\":%llu,"
            "\"stateBudgetLimitedTicks\":%llu,\"controlBudgetLimitedTicks\":%llu,"
            "\"stateBackpressureCount\":%llu,\"controlBackpressureCount\":%llu,"
            "\"preparedStateItems\":%llu,\"lastTickDurationMicroseconds\":%llu,"
            "\"maxTickDurationMicroseconds\":%llu,\"udpReceivedDatagrams\":%llu,\"udpReceivedBytes\":%llu,"
            "\"udpSentDatagrams\":%llu,\"udpSentBytes\":%llu,\"udpRejectedDatagrams\":%llu,"
            "\"udpSendWouldBlock\":%llu,\"udpSocketErrors\":%llu}\n", kind,
            static_cast<unsigned long long>(elapsedMilliseconds), activeSessions,
            static_cast<unsigned long long>(receivedFrames),
            static_cast<unsigned long long>(queuedSendFrames),
            static_cast<unsigned long long>(errors), pendingReceiveBytes, pendingJobs,
            pendingParseBytes, pendingParseTasks, queuedSendBytes,
            static_cast<unsigned long long>(aoi.joinedPlayers),
            static_cast<unsigned long long>(aoi.positionedPlayers),
            static_cast<unsigned long long>(aoi.visibleEdges),
            static_cast<unsigned long long>(aoi.coalescedStates),
            static_cast<unsigned long long>(aoi.sentStateItems),
            static_cast<unsigned long long>(aoi.sentStateFrames),
            static_cast<unsigned long long>(aoi.tickCount),
            static_cast<unsigned long long>(aoi.sentStateBytes),
            static_cast<unsigned long long>(aoi.sentControlBytes),
            static_cast<unsigned long long>(aoi.lastTickStateBytes),
            static_cast<unsigned long long>(aoi.lastTickControlBytes),
            static_cast<unsigned long long>(aoi.stateBudgetLimitedTicks),
            static_cast<unsigned long long>(aoi.controlBudgetLimitedTicks),
            static_cast<unsigned long long>(aoi.stateBackpressureCount),
            static_cast<unsigned long long>(aoi.controlBackpressureCount),
            static_cast<unsigned long long>(aoi.preparedStateItems),
            static_cast<unsigned long long>(aoi.lastTickDurationMicroseconds),
            static_cast<unsigned long long>(aoi.maxTickDurationMicroseconds),
            static_cast<unsigned long long>(udp.receivedDatagrams),
            static_cast<unsigned long long>(udp.receivedBytes),
            static_cast<unsigned long long>(udp.sentDatagrams),
            static_cast<unsigned long long>(udp.sentBytes),
            static_cast<unsigned long long>(udp.rejectedDatagrams),
            static_cast<unsigned long long>(udp.sendWouldBlock),
            static_cast<unsigned long long>(udp.socketErrors));
        std::fflush(stdout);
    }
};

class MetricsReporter
{
public:
    explicit MetricsReporter(const std::uint32_t intervalMilliseconds)
        : mIntervalMilliseconds(intervalMilliseconds)
        , mStartedAt(ServerCore::Core::MillisecondsSinceProcessStart())
    {
    }

    void Collect(const ServerCore::Runtime::ServerHost& host,
        Summit::SummitServerBackend& backend, const Summit::SummitUdpTransport& udp,
        const std::uint64_t now) noexcept
    {
        if (mIntervalMilliseconds == 0 ||
            now - mLastSampleAt < mIntervalMilliseconds)
        {
            return;
        }
        mLastSampleAt = now;
        // SnapshotMetrics는 JobRunner 문맥 전용이며 세션별 배열을 일시 할당한다.
        // 누락된 주기를 몰아서 출력하지 않고 이 한 샘플의 합계만 보관한다.
        try
        {
            const auto snapshot = host.SnapshotMetrics();
            if (!snapshot.IsOk()) return;
            const auto& values = snapshot.Value();
            MetricsSample sample{ now - mStartedAt, values.activeSessionCount,
                values.receivedFrameCount, values.queuedSendFrameCount, values.errorCount,
                values.pendingReceiveBytes, values.pendingJobCount, values.pendingParseBytes,
                values.pendingParseTaskCount };
            for (const auto& session : values.sessionSendQueues) sample.queuedSendBytes += session.queuedBytes;
            // Host 표본과 AOI 표본은 서로 다른 경계지만 모두 이 JobRunner에서 요청한다.
            // 가시 관계 수는 현재값이고 병합/송신/tick은 백엔드 수명 동안의 누계다.
            sample.aoi = backend.SnapshotAoiMetrics();
            sample.udp = udp.SnapshotMetrics();
            mLastSample = sample;
            sample.Print("metrics");
        }
        catch (...)
        {
            // 관측 실패 때문에 직렬 게임 처리기의 no-throw 계약을 깨지 않는다.
        }
    }

    void PrintLast() const noexcept
    {
        // Run 종료 뒤에는 JobRunner에 새 snapshot 요청을 넣을 수 없다. 이 값은 종료 완료
        // 시점의 active=0 보장이 아니라, 마지막으로 관측한 시점임을 종류로 명시한다.
        if (mLastSample) mLastSample->Print("last_sample_before_stop");
    }

private:
    std::uint32_t mIntervalMilliseconds;
    std::uint64_t mStartedAt;
    std::uint64_t mLastSampleAt = mStartedAt;
    std::optional<MetricsSample> mLastSample;
};

BOOL WINAPI HandleConsoleControl(const DWORD controlType)
{
    if (controlType != CTRL_C_EVENT && controlType != CTRL_BREAK_EVENT &&
        controlType != CTRL_CLOSE_EVENT)
    {
        return FALSE;
    }

    if (const std::shared_ptr<ServerCore::Runtime::ServerHost> host =
            gRunningHost.load(std::memory_order_acquire))
    {
        host->Stop();
        return TRUE;
    }
    return FALSE;
}
}

int main(const int argc, char* argv[])
{
    // 콘솔에서도 콘텐츠와 같은 UTF-8 바이트를 표시한다. 리디렉션한 로그 파일은 변환하지 않는다.
    (void)SetConsoleOutputCP(CP_UTF8);
    CommandLineOptions commandLine;
    if (!ParseCommandLine(argc, argv, commandLine))
    {
        PrintUsage();
        return 2;
    }
    if (commandLine.showHelp)
    {
        PrintUsage();
        return 0;
    }

    const std::shared_ptr<ServerCore::Runtime::ServerHost> host =
        std::make_shared<ServerCore::Runtime::ServerHost>();
    host->SetLogger(std::make_shared<ConsoleLogger>());
    ServerCore::Runtime::ServerHostOptions hostOptions;
    hostOptions.listenAddress = commandLine.address;
    hostOptions.port = commandLine.port;
    hostOptions.acceptBacklog = static_cast<int>(commandLine.maxConnections);
    hostOptions.maxConcurrentSessions = commandLine.maxConnections;
    hostOptions.maxBodySize = Summit::SummitServerBackend::MaximumBodyBytes;
    // 큰 용량도 프레임/공유 송수신 예산을 끄지 않는다. 65,536개 FrameReader의 상한만
    // (8192 + 4) * 65536 = 512.25 MiB이며, 실제 연결과 큐에는 별도 메모리가 필요하다.
    // UDP movement does not refresh the TCP receive clock. Negotiated clients
    // send a small TCP Heartbeat every five seconds, including while stationary.
    hostOptions.idleSessionTimeout = std::chrono::seconds(30);

    const ServerCore::Core::Status configured = host->Configure(hostOptions);
    if (!configured.IsOk())
    {
        PrintFailure("Server configuration", configured);
        return 1;
    }

    const auto udp = std::make_shared<Summit::SummitUdpTransport>();
    const auto udpBound = udp->Bind(commandLine.address, commandLine.port);
    if (!udpBound.IsOk())
    {
        PrintFailure("UDP listener", udpBound);
        return 1;
    }
    const std::shared_ptr<Summit::SummitServerBackend> backend =
        std::make_shared<Summit::SummitServerBackend>(
            commandLine.maxPlayers, commandLine.maxConnections, commandLine.scheduler);
    backend->SetMovementTransport(udp);
    ServerCore::Dispatch::Dispatcher& dispatcher = host->GetDispatcher();
    dispatcher.SetUnknownTypePolicy(ServerCore::Dispatch::UnknownTypePolicy::Disconnect);
    const ServerCore::Core::Status registered = backend->RegisterHandlers(dispatcher);
    if (!registered.IsOk())
    {
        PrintFailure("Handler registration", registered);
        return 1;
    }
    host->SetSessionObserver(backend);

    const ServerCore::Core::Status started = host->Start();
    if (!started.IsOk())
    {
        PrintFailure("Server start", started);
        return 1;
    }

    // 유휴 제한은 수신할 때마다 갱신된다. 별도의 절대 가입 기한으로, 바이트를 조금씩 보내는
    // 미가입 클라이언트도 정리하며 콜백은 게임 처리기와 같은 JobRunner에서 실행한다.
    const std::weak_ptr<Summit::SummitServerBackend> weakBackend = backend;
    const std::weak_ptr<ServerCore::Runtime::ServerHost> weakHost = host;
    ServerCore::Runtime::PeriodicRunner udpReceives(
        host->GetJobRunner(), std::chrono::milliseconds(10), [weakBackend, udp]() {
            udp->Poll([weakBackend](const auto id, const auto& message) {
                if (const auto current = weakBackend.lock()) current->ReceiveMovement(id, message);
            });
        });
    const auto udpStarted = udpReceives.Start();
    if (!udpStarted.IsOk())
    {
        host->Stop();
        PrintFailure("UDP receive scheduler", udpStarted);
        return 1;
    }
    const auto metrics = std::make_shared<MetricsReporter>(commandLine.metricsIntervalMilliseconds);
    ServerCore::Runtime::PeriodicRunner joinTimeouts(
        host->GetJobRunner(), std::chrono::milliseconds(250), [weakBackend, weakHost, metrics, udp]() {
            const std::uint64_t now = ServerCore::Core::MillisecondsSinceProcessStart();
            const std::shared_ptr<Summit::SummitServerBackend> current = weakBackend.lock();
            if (current)
            {
                current->ExpireUnjoinedSessions(now);
                if (const auto currentHost = weakHost.lock()) metrics->Collect(*currentHost, *current, *udp, now);
            }
        });
    const ServerCore::Core::Status timerStarted = joinTimeouts.Start();
    if (!timerStarted.IsOk())
    {
        host->Stop();
        PrintFailure("Join timeout scheduler", timerStarted);
        return 1;
    }

    // 상태 복제는 가입 기한 검사와 별도 주기를 쓴다. 같은 JobRunner에서 처리하며,
    // 이전 tick이 밀려 있으면 PeriodicRunner가 새 예약을 생략해 과거 상태를 몰아 보내지 않는다.
    ServerCore::Runtime::PeriodicRunner aoiUpdates(
        host->GetJobRunner(), std::chrono::milliseconds(50), [weakBackend]() {
            if (const auto current = weakBackend.lock())
                current->Tick(ServerCore::Core::MillisecondsSinceProcessStart());
        });
    const auto aoiStarted = aoiUpdates.Start();
    if (!aoiStarted.IsOk())
    {
        joinTimeouts.Stop();
        host->Stop();
        PrintFailure("AOI update scheduler", aoiStarted);
        return 1;
    }

    gRunningHost.store(host, std::memory_order_release);
    if (SetConsoleCtrlHandler(HandleConsoleControl, TRUE) == FALSE)
    {
        gRunningHost.store(nullptr, std::memory_order_release);
        aoiUpdates.Stop();
        joinTimeouts.Stop();
        host->Stop();
        std::cerr << "Could not install the console shutdown handler.\n";
        return 1;
    }

    Summit::ServerConsole console(GetStdHandle(STD_INPUT_HANDLE),
        MakeConsoleHandler(host->GetJobRunner(), backend));
    const auto consoleStarted = console.Start();
    if (!consoleStarted.IsOk())
    {
        gRunningHost.store(nullptr, std::memory_order_release);
        (void)SetConsoleCtrlHandler(HandleConsoleControl, FALSE);
        aoiUpdates.Stop();
        joinTimeouts.Stop();
        host->Stop();
        PrintFailure("Console input", consoleStarted);
        return 1;
    }

    std::cout << "SummitServer listening on " << commandLine.address << ':' << host->Port()
              << " (players " << commandLine.maxPlayers << ", connections " << commandLine.maxConnections << ")\n"
              << "Movement UDP listening on " << commandLine.address << ':' << udp->Port() << " (max datagram 1200 bytes)\n"
              << "Tick budgets (bytes): state/client=" << commandLine.scheduler.perClientStateBytesPerTick
              << ", state/total=" << commandLine.scheduler.globalStateBytesPerTick
              << ", directory-AOI/client=" << commandLine.scheduler.perClientControlBytesPerTick
              << ", directory-AOI/total=" << commandLine.scheduler.globalControlBytesPerTick << '\n'
              << "Console: /announce <message> sends a server notice; /players lists joined players; /help lists commands.\n"
              << "Press Ctrl+C to stop.\n" << std::flush;

    const int exitCode = host->Run();
    console.Stop();
    gRunningHost.store(nullptr, std::memory_order_release);
    (void)SetConsoleCtrlHandler(HandleConsoleControl, FALSE);
    aoiUpdates.Stop();
    joinTimeouts.Stop();
    udpReceives.Stop();
    udp->Close();
    metrics->PrintLast();
    return exitCode;
}
