#pragma once

#include "ServerCore/Core/Error.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace Summit
{
/// <summary>콘솔의 UTF-16 입력 또는 리디렉션된 UTF-8 입력을 한 줄씩 전달한다.</summary>
/// <remarks>
/// 입력 HANDLE은 빌린다. 소유자는 Stop 반환까지 핸들을 유지한다. Start/Stop은 소유 스레드에서
/// 호출하며, 콜백은 입력 스레드에서 실행된다. EOF는 입력만 끝내고 서버 종료를 요청하지 않는다.
/// Stop은 대기 중인 동기 I/O를 취소하고 join한다. 콜백은 오래 기다리지 않아야 한다.
/// </remarks>
class ServerConsole final
{
public:
    using LineHandler = std::function<void(std::string)>;

    ServerConsole(void* inputHandle, LineHandler handler);
    ~ServerConsole();
    ServerConsole(const ServerConsole&) = delete;
    ServerConsole& operator=(const ServerConsole&) = delete;

    [[nodiscard]] ServerCore::Core::Status Start();
    void Stop() noexcept;

private:
    void ReadInput() noexcept;

    void* mInputHandle;
    LineHandler mHandler;
    std::atomic<bool> mStopRequested{ false };
    std::thread mThread;
    bool mStarted = false;
};
}
