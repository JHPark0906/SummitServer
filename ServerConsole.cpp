#include "ServerConsole.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <array>
#include <cstdio>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>

namespace Summit
{
namespace
{
// 공지는 최대 512 UTF-8 바이트지만 명령 접두사와 잘못된 입력도 제한된 저장소에서 처리한다.
constexpr std::size_t MaximumLineUnits = 2048;

template <typename Character, typename Callback>
void ConsumeLines(const std::basic_string_view<Character> input,
    std::basic_string<Character>& pending, bool& oversized, Callback&& callback)
{
    for (const Character character : input)
    {
        if (character == static_cast<Character>('\n'))
        {
            if (!oversized)
            {
                if (!pending.empty() && pending.back() == static_cast<Character>('\r')) pending.pop_back();
                callback(pending);
            }
            else std::fputs("Console command rejected: line is too long.\n", stderr);
            pending.clear();
            oversized = false;
        }
        else if (!oversized)
        {
            if (pending.size() == MaximumLineUnits)
            {
                pending.clear();
                oversized = true;
            }
            else pending.push_back(character);
        }
    }
}
}

ServerConsole::ServerConsole(void* inputHandle, LineHandler handler)
    : mInputHandle(inputHandle), mHandler(std::move(handler))
{
}

ServerConsole::~ServerConsole()
{
    Stop();
}

ServerCore::Core::Status ServerConsole::Start()
{
    using namespace ServerCore::Core;
    if (mStarted) return Status::Fail(ErrorCode::AlreadyExists, "Console input already started");
    if (!mHandler) return Status::Fail(ErrorCode::InvalidArgument, "Console input requires a handler");
    try
    {
        mThread = std::thread([this]() { ReadInput(); });
        mStarted = true;
        return Status::Ok();
    }
    catch (const std::bad_alloc&)
    {
        return Status::AllocationFailure();
    }
    catch (const std::system_error&)
    {
        return Status::FailWithoutMessage(ErrorCode::PlatformError);
    }
}

void ServerConsole::Stop() noexcept
{
    mStopRequested.store(true, std::memory_order_release);
    if (!mThread.joinable()) return;
    const HANDLE thread = mThread.native_handle();
    // 취소 한 번만 하면 stop 검사 직후 새 ReadFile/ReadConsoleW가 시작하는 경합을 놓친다.
    // 취소는 완료 대기가 아니므로 실제 스레드 종료까지 반복하고 나서 thread 소유권을 거둔다.
    while (WaitForSingleObject(thread, 0) == WAIT_TIMEOUT)
    {
        (void)CancelSynchronousIo(thread);
        (void)WaitForSingleObject(thread, 20);
    }
    mThread.join();
}

void ServerConsole::ReadInput() noexcept
{
    try
    {
        const HANDLE input = static_cast<HANDLE>(mInputHandle);
        if (input == nullptr || input == INVALID_HANDLE_VALUE) return;
        DWORD mode = 0;
        const bool console = GetConsoleMode(input, &mode) != FALSE;
        bool oversized = false;
        bool firstLine = true;
        const auto deliver = [this, &firstLine](std::string line) {
            if (firstLine && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
            firstLine = false;
            if (!mStopRequested.load(std::memory_order_acquire)) mHandler(std::move(line));
        };
        std::string pendingBytes;
        std::wstring pendingWide;
        const auto deliverWide = [&deliver](const std::wstring& line) {
            if (line.empty()) { deliver({}); return; }
            const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(),
                static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
            if (length <= 0)
            {
                std::fputs("Console command rejected: invalid Unicode.\n", stderr);
                return;
            }
            std::string utf8(static_cast<std::size_t>(length), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(),
                    static_cast<int>(line.size()), utf8.data(), length, nullptr, nullptr) == length)
                deliver(std::move(utf8));
        };
        while (!mStopRequested.load(std::memory_order_acquire))
        {
            DWORD count = 0;
            BOOL read = FALSE;
            if (console)
            {
                std::array<wchar_t, 256> buffer{};
                read = ReadConsoleW(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr);
                if (read && count != 0)
                    ConsumeLines(std::wstring_view(buffer.data(), count), pendingWide, oversized, deliverWide);
            }
            else
            {
                std::array<char, 256> buffer{};
                read = ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr);
                if (read && count != 0)
                    ConsumeLines(std::string_view(buffer.data(), count), pendingBytes, oversized, deliver);
            }
            if (!read || count == 0)
            {
                // 파이프 EOF나 파일 끝은 더 읽을 것이 없다는 뜻이다. 취소한 부분 줄은 실행하지 않는다.
                if (!mStopRequested.load(std::memory_order_acquire) && !oversized &&
                    (read || GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_HANDLE_EOF))
                {
                    if (console && !pendingWide.empty()) deliverWide(pendingWide);
                    else if (!console && !pendingBytes.empty()) deliver(std::move(pendingBytes));
                }
                return;
            }
        }
    }
    catch (...)
    {
        std::fputs("Console input stopped after an input-processing failure.\n", stderr);
    }
}
}
