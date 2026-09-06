#include "ServerConsole.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;

void Check(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    void Close() { if (value) CloseHandle(value); value = nullptr; }
};

struct Lines
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> values;

    void Add(std::string line)
    {
        const std::lock_guard guard(mutex);
        values.push_back(std::move(line));
        changed.notify_all();
    }
    void Await(const std::size_t count)
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock, 3s, [&]() { return values.size() >= count; }), "Input lines did not arrive");
    }
};

void Write(const HANDLE output, const std::string_view bytes)
{
    DWORD written = 0;
    Check(WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size(), "Pipe write failed");
}

void TestPipeLines()
{
    Handle input;
    Handle output;
    Check(CreatePipe(&input.value, &output.value, nullptr, 4096) != FALSE, "CreatePipe failed");
    Lines lines;
    Summit::ServerConsole console(input.value, [&](std::string line) { lines.Add(std::move(line)); });
    Check(console.Start().IsOk(), "Console input could not start");
    const std::string first = "\xEF\xBB\xBF/announce 한글 공지 😀\r\n";
    for (const char byte : first) Write(output.value, std::string_view(&byte, 1));
    Write(output.value, std::string(2200, 'x') + "\n/announce after oversized\n/players\r\n/announce final without newline");
    output.Close();
    lines.Await(4);
    console.Stop();
    Check(lines.values == std::vector<std::string>{ "/announce 한글 공지 😀", "/announce after oversized", "/players",
        "/announce final without newline" }, "Split UTF-8, CRLF, oversized recovery or EOF changed input");
}

void TestPendingReadCancellation()
{
    // 열린 writer가 아무것도 보내지 않아도 Stop은 입력 완료나 EOF를 기다리지 않아야 한다.
    for (int iteration = 0; iteration < 20; ++iteration)
    {
        Handle input;
        Handle output;
        Check(CreatePipe(&input.value, &output.value, nullptr, 4096) != FALSE, "CreatePipe failed");
        Lines lines;
        Summit::ServerConsole console(input.value, [&](std::string line) { lines.Add(std::move(line)); });
        Check(console.Start().IsOk(), "Console input could not start");
        if (iteration != 0)
        {
            Write(output.value, "/help\n/announce incomplete");
            lines.Await(1);
        }
        const auto started = std::chrono::steady_clock::now();
        console.Stop();
        Check(std::chrono::steady_clock::now() - started < 2s, "Pending ReadFile did not cancel promptly");
        Check(lines.values.size() == (iteration == 0 ? 0u : 1u), "Stop executed an incomplete command");
    }
}

void TestWideConsoleChild()
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    Check(GetConsoleMode(input, &mode) != FALSE, "Child did not receive its own console");
    Lines lines;
    Summit::ServerConsole console(input, [&](std::string line) { lines.Add(std::move(line)); });
    Check(console.Start().IsOk(), "Wide console input could not start");
    const std::wstring command = L"/announce 한글 공지 \U0001F600\r";
    for (const wchar_t character : command)
    {
        INPUT_RECORD event{};
        event.EventType = KEY_EVENT;
        event.Event.KeyEvent.bKeyDown = TRUE;
        event.Event.KeyEvent.wRepeatCount = 1;
        event.Event.KeyEvent.uChar.UnicodeChar = character;
        if (character == L'\r') event.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        DWORD written = 0;
        Check(WriteConsoleInputW(input, &event, 1, &written) && written == 1, "Wide console injection failed");
    }
    lines.Await(1);
    const auto started = std::chrono::steady_clock::now();
    console.Stop();
    Check(std::chrono::steady_clock::now() - started < 2s, "Pending ReadConsoleW did not cancel promptly");
    Check(lines.values == std::vector<std::string>{ "/announce 한글 공지 😀" }, "ReadConsoleW lost Unicode characters");
}

void RunHiddenConsoleChild()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    Check(length != 0 && length < path.size(), "Cannot find the test executable");
    path.resize(length);
    std::wstring command = L"\"" + path + L"\" --wide-console-child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    // 전용 숨은 콘솔만 생성한다. 부모나 사용자의 콘솔 입력/IME 설정에는 접근하지 않는다.
    Check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
        nullptr, nullptr, &startup, &process) != FALSE, "Cannot start the hidden console test");
    Handle child{ process.hProcess };
    Handle childThread{ process.hThread };
    if (WaitForSingleObject(child.value, 8000) != WAIT_OBJECT_0)
    {
        (void)TerminateProcess(child.value, 1);
        (void)WaitForSingleObject(child.value, 3000);
        throw std::runtime_error("Hidden console child timed out");
    }
    DWORD exitCode = 1;
    Check(GetExitCodeProcess(child.value, &exitCode) && exitCode == 0, "Hidden Unicode console test failed");
}
}

int main(const int argc, char* argv[])
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--wide-console-child") TestWideConsoleChild();
        else
        {
            TestPipeLines();
            TestPendingReadCancellation();
            RunHiddenConsoleChild();
        }
        std::puts("Server console input tests passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
