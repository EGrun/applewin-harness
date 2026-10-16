#pragma once
// Helpers for tests that launch AppleWinHarness.exe --serve and talk to it over its named pipe.
#include "support/golden.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace harness::testsupport {

// A running AppleWinHarness.exe (next to the test exe) with stdout and stderr drained on helper threads. The destructor
// terminates the child if it is still running, so failure paths never leak a process.
class ServerProcess {
public:
    explicit ServerProcess(const std::wstring& args)
    {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE outWr = nullptr, errWr = nullptr;
        if (!CreatePipe(&outRd_, &outWr, &sa, 0)) return;
        SetHandleInformation(outRd_, HANDLE_FLAG_INHERIT, 0);
        if (!CreatePipe(&errRd_, &errWr, &sa, 0)) {
            CloseHandle(outWr);
            return;
        }
        SetHandleInformation(errRd_, HANDLE_FLAG_INHERIT, 0);
        const auto exe = TestExeDir() / "AppleWinHarness.exe";
        std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = outWr;
        si.hStdError = errWr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       TestExeDir().c_str(), &si, &pi_);
        CloseHandle(outWr);
        CloseHandle(errWr);
        if (!ok) {
            pi_ = {};
            return;
        }
        outReader_ = std::thread([this] { Drain(outRd_, out_, outDone_); });
        errReader_ = std::thread([this] { Drain(errRd_, err_, errDone_); });
    }

    ~ServerProcess()
    {
        Kill();
        if (outReader_.joinable()) outReader_.join();
        if (errReader_.joinable()) errReader_.join();
        if (outRd_) CloseHandle(outRd_);
        if (errRd_) CloseHandle(errRd_);
        if (pi_.hProcess) CloseHandle(pi_.hProcess);
        if (pi_.hThread) CloseHandle(pi_.hThread);
    }

    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    bool Launched() const { return pi_.hProcess != nullptr; }
    DWORD Pid() const { return pi_.dwProcessId; }
    bool Running() const { return pi_.hProcess && WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT; }

    // The first stdout line without its line end, once it is complete; nullopt on timeout or if stdout closed first.
    std::optional<std::string> WaitForLine(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [this] { return out_.find('\n') != std::string::npos || outDone_; });
        const size_t nl = out_.find('\n');
        if (nl == std::string::npos) return std::nullopt;
        std::string line = out_.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }

    std::string Stdout() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return out_;
    }

    std::string Stderr() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return err_;
    }

    // The exit code once the child has exited, or nullopt after timeout (the child keeps running).
    std::optional<DWORD> WaitExit(std::chrono::milliseconds timeout)
    {
        if (!Launched()) return std::nullopt;
        if (WaitForSingleObject(pi_.hProcess, DWORD(timeout.count())) != WAIT_OBJECT_0) return std::nullopt;
        DWORD code = 0;
        GetExitCodeProcess(pi_.hProcess, &code);
        return code;
    }

    void Kill()
    {
        if (Running()) {
            TerminateProcess(pi_.hProcess, 1);
            WaitForSingleObject(pi_.hProcess, 10000);
        }
    }

private:
    void Drain(HANDLE rd, std::string& sink, bool& done)
    {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            sink.append(buf, got);
            cv_.notify_all();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        done = true;
        cv_.notify_all();
    }

    PROCESS_INFORMATION pi_{};
    HANDLE outRd_ = nullptr, errRd_ = nullptr;
    std::thread outReader_, errReader_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string out_, err_;
    bool outDone_ = false, errDone_ = false;
};

// The announcement line's fields (spec: docs/specs/m3.md, Part A "Launch").
struct Announcement {
    std::wstring pipe;
    std::string agentToken, operatorToken;
    uint64_t pid = 0;
};

inline std::optional<Announcement> ParseAnnouncement(const std::string& line)
{
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions*/ false);
    if (!j.is_object() || !j.contains("pipe") || !j["pipe"].is_string() || !j.contains("agent_token") ||
        !j["agent_token"].is_string() || !j.contains("operator_token") || !j["operator_token"].is_string() ||
        !j.contains("pid") || !j["pid"].is_number_unsigned())
        return std::nullopt;
    Announcement a;
    const std::string pipe = j["pipe"].get<std::string>();
    a.pipe.assign(pipe.begin(), pipe.end());   // ASCII
    a.agentToken = j["agent_token"].get<std::string>();
    a.operatorToken = j["operator_token"].get<std::string>();
    a.pid = j["pid"].get<uint64_t>();
    return a;
}

// A client end of the harness pipe, with overlapped I/O so every read and write has a timeout.
class PipeClient {
public:
    enum class Status { Ok, Timeout, Closed };

    PipeClient() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~PipeClient()
    {
        Close();
        if (event_) CloseHandle(event_);
    }
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // Retries while every instance is busy, up to timeout.
    bool Open(const std::wstring& name, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            h_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                             nullptr);
            if (h_ != INVALID_HANDLE_VALUE) return true;
            h_ = nullptr;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 100);
            else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    HANDLE Handle() const { return h_; }

    // The process id of the pipe's server end (0 if unknown): clients compare it with the announced pid before hello.
    DWORD ServerPid() const
    {
        ULONG pid = 0;
        return h_ && GetNamedPipeServerProcessId(h_, &pid) ? DWORD(pid) : 0;
    }

    void Close()
    {
        if (h_) {
            CancelIoEx(h_, nullptr);
            CloseHandle(h_);
            h_ = nullptr;
        }
    }

    bool WriteRaw(const void* data, size_t size, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
    {
        if (!h_) return false;
        OVERLAPPED ov{};
        ov.hEvent = event_;
        ResetEvent(event_);
        DWORD done = 0;
        if (!WriteFile(h_, data, DWORD(size), &done, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(event_, DWORD(timeout.count())) != WAIT_OBJECT_0) {
                CancelIoEx(h_, &ov);
                GetOverlappedResult(h_, &ov, &done, TRUE);
                return false;
            }
            if (!GetOverlappedResult(h_, &ov, &done, FALSE)) return false;
        }
        return done == size;
    }

    static std::vector<uint8_t> Frame(const std::string& body)
    {
        const uint32_t n = uint32_t(body.size());
        std::vector<uint8_t> wire = {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
        wire.insert(wire.end(), body.begin(), body.end());
        return wire;
    }

    bool SendBody(const std::string& body)
    {
        const auto wire = Frame(body);
        return WriteRaw(wire.data(), wire.size());
    }

    bool Send(const nlohmann::json& message) { return SendBody(message.dump()); }

    // One whole message. A body that is not JSON is reported as Closed (the server never sends one).
    Status Read(nlohmann::json& out, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        uint8_t prefix[4];
        Status s = ReadExact(prefix, 4, deadline);
        if (s != Status::Ok) return s;
        const uint32_t n = uint32_t(prefix[0]) | (uint32_t(prefix[1]) << 8) | (uint32_t(prefix[2]) << 16) |
                           (uint32_t(prefix[3]) << 24);
        if (n == 0 || n > 16u * 1024u * 1024u) return Status::Closed;
        std::string body(n, '\0');
        // The rest of a started message gets extra time: the server writes messages whole.
        s = ReadExact(reinterpret_cast<uint8_t*>(body.data()), n,
                      (std::max)(deadline,std::chrono::steady_clock::now() + std::chrono::seconds(10)));
        if (s != Status::Ok) return Status::Closed;
        out = nlohmann::json::parse(body, nullptr, /*allow_exceptions*/ false);
        return out.is_discarded() ? Status::Closed : Status::Ok;
    }

    // Sends a request with the next id and returns the reply with that id (notifications are kept for
    // Notifications()); nullopt if the connection closed or nothing arrived within timeout.
    std::optional<nlohmann::json> Call(const std::string& method, const nlohmann::json& params = nlohmann::json::object(),
                                       std::chrono::milliseconds timeout = std::chrono::milliseconds(30000))
    {
        const int64_t id = nextId_++;
        if (!Send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}})) return std::nullopt;
        return ReadReply(id, timeout);
    }

    // The next message that carries an id (any id, including null), keeping notifications aside.
    std::optional<nlohmann::json> ReadAnyReply(std::chrono::milliseconds timeout = std::chrono::milliseconds(10000))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;
            nlohmann::json m;
            if (Read(m, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)) != Status::Ok)
                return std::nullopt;
            if (m.is_object() && m.contains("id")) return m;
            notifications_.push_back(std::move(m));
        }
    }

    std::optional<nlohmann::json> ReadReply(const nlohmann::json& id, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;
            auto m = ReadAnyReply(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            if (!m) return std::nullopt;
            if ((*m)["id"] == id) return m;
        }
    }

    // True once the server has closed the connection (messages that arrive first are discarded).
    bool WaitClosed(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            nlohmann::json m;
            const Status s = Read(m, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            if (s == Status::Closed) return true;
            if (s == Status::Timeout) return false;
        }
    }

    // True if the server has not closed the connection within the given time (messages that arrive are kept).
    bool StaysOpen(std::chrono::milliseconds time = std::chrono::milliseconds(500))
    {
        const auto deadline = std::chrono::steady_clock::now() + time;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return true;
            nlohmann::json m;
            const Status s = Read(m, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            if (s == Status::Closed) return false;
            if (s == Status::Timeout) return true;
            notifications_.push_back(std::move(m));
        }
    }

    const std::vector<nlohmann::json>& Notifications() const { return notifications_; }

private:
    Status ReadExact(uint8_t* p, size_t n, std::chrono::steady_clock::time_point deadline)
    {
        if (!h_) return Status::Closed;
        size_t have = 0;
        while (have < n) {
            const auto now = std::chrono::steady_clock::now();
            const DWORD waitMs = now >= deadline ? 0 : DWORD(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            OVERLAPPED ov{};
            ov.hEvent = event_;
            ResetEvent(event_);
            DWORD got = 0;
            if (!ReadFile(h_, p + have, DWORD(n - have), &got, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) return Status::Closed;
                if (WaitForSingleObject(event_, waitMs) != WAIT_OBJECT_0) {
                    CancelIoEx(h_, &ov);
                    if (!GetOverlappedResult(h_, &ov, &got, TRUE) || got == 0) return Status::Timeout;
                } else if (!GetOverlappedResult(h_, &ov, &got, FALSE)) {
                    return Status::Closed;
                }
            }
            if (got == 0) return Status::Closed;
            have += got;
        }
        return Status::Ok;
    }

    HANDLE h_ = nullptr;
    HANDLE event_ = nullptr;
    int64_t nextId_ = 1;
    std::vector<nlohmann::json> notifications_;
};

// The top-level window of class AppleWinHarnessWindow owned by process pid, or nullptr.
inline HWND FindHarnessWindowOf(DWORD pid)
{
    struct Search {
        DWORD pid;
        HWND found;
    } s{pid, nullptr};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(lp);
            DWORD owner = 0;
            GetWindowThreadProcessId(h, &owner);
            if (owner != s.pid) return TRUE;
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, int(std::size(cls)));
            if (std::wstring(cls) != L"AppleWinHarnessWindow") return TRUE;
            s.found = h;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.found;
}

}  // namespace harness::testsupport
