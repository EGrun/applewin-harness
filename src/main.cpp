#include "host/emulator.h"
#include "core/png_writer.h"
#include "core/sha256.h"
#include "core/working_copy.h"
#include "server/pipe_server.h"
#include "server/protocol.h"
#include "server/session.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <tlhelp32.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

struct Step {
    enum Kind { Frames, Type } kind = Frames;
    uint32_t frames = 0;
    std::string text;
};

struct Args {
    bool serve = false;
    bool headless = false;
    bool realtime = false;
    std::optional<std::filesystem::path> disk1;
    std::optional<std::filesystem::path> png;
    std::vector<Step> steps;
};

int Usage()
{
    std::cerr << "usage: AppleWinHarness.exe [--headless] [--realtime] [--disk1 <image>] [--png <path>] [step ...]\n"
                 "       AppleWinHarness.exe --serve [--headless]\n"
                 "step: --frames <n> | --type <text>\n"
                 "  --type escapes: \\r and \\n = Return, \\\\ = backslash; text is 0x20-0x7E.\n"
                 "  --headless needs at least one step; without steps a window runs until closed.\n";
    return kExitUsage;
}

bool ParseFrames(const std::wstring& s, uint32_t& out)
{
    if (s.empty() || s.size() > 9) return false;
    uint32_t v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + uint32_t(c - L'0');
    }
    out = v;
    return true;
}

// Decodes escapes and narrows to bytes; rejects empty text, bad escapes and characters outside 0x20-0x7E.
bool ParseTypeText(const std::wstring& s, std::string& out)
{
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'\\') {
            if (++i >= s.size()) return false;
            if (s[i] == L'r' || s[i] == L'n') out.push_back('\r');
            else if (s[i] == L'\\') out.push_back('\\');
            else return false;
        } else if (c >= 0x20 && c <= 0x7E) {
            out.push_back(char(c));
        } else {
            return false;
        }
    }
    return !out.empty();
}

bool ParseArgs(int argc, wchar_t** argv, Args& a)
{
    bool haveHeadless = false, haveRealtime = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring k = argv[i];
        const bool hasValue = i + 1 < argc;
        if (k == L"--serve" && !a.serve) {
            a.serve = true;
        } else if (k == L"--headless" && !haveHeadless) {
            haveHeadless = a.headless = true;
        } else if (k == L"--realtime" && !haveRealtime) {
            haveRealtime = a.realtime = true;
        } else if (k == L"--disk1" && hasValue && !a.disk1) {
            a.disk1 = std::filesystem::path(argv[++i]);
        } else if (k == L"--png" && hasValue && !a.png) {
            a.png = std::filesystem::path(argv[++i]);
        } else if (k == L"--frames" && hasValue) {
            Step s;
            s.kind = Step::Frames;
            if (!ParseFrames(argv[++i], s.frames)) return false;
            a.steps.push_back(std::move(s));
        } else if (k == L"--type" && hasValue) {
            Step s;
            s.kind = Step::Type;
            if (!ParseTypeText(argv[++i], s.text)) return false;
            a.steps.push_back(std::move(s));
        } else {
            return false;
        }
    }
    if (a.serve)   // --serve [--headless] only (spec: docs/specs/m3.md, Part A "Launch")
        return !a.realtime && !a.disk1 && !a.png && a.steps.empty();
    return !(a.headless && a.steps.empty());
}

// Throws on failure; returns false if the window was closed.
bool RunSteps(harness::Emulator& emu, const Args& a)
{
    const auto pacing = a.realtime ? harness::Pacing::Realtime : harness::Pacing::Stepped;
    for (const Step& s : a.steps) {
        bool open = s.kind == Step::Frames ? emu.RunFrames(s.frames, pacing) : emu.TypeText(s.text, pacing);
        if (!open) return false;
    }
    return true;
}

// Shutdown ejects the disks (AppleWin then closes the working copy, so it can be deleted).
void ReleaseEmulator(harness::Emulator& emu)
{
    try { emu.Shutdown(); } catch (...) {}
}

// Ruling D11: a harness that was killed leaves %TEMP%\applewin-harness\<pid> and <pid>.ini behind. At startup, entries named
// for a pid that is no longer running are removed (never throws). Live pids, this process and reparse points (junctions,
// symbolic links: never followed) are skipped.
bool ProcessIsRunning(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() != ERROR_INVALID_PARAMETER;   // access denied: it exists
    DWORD code = 0;
    const bool running = !GetExitCodeProcess(h, &code) || code == STILL_ACTIVE;
    CloseHandle(h);
    return running;
}

void SweepStaleWorkDirs() noexcept
{
    namespace fs = std::filesystem;
    try {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "applewin-harness";
        if (ec || !fs::is_directory(root, ec)) return;
        const DWORD self = GetCurrentProcessId();
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path p = it->path();
            std::string name = p.filename().string();
            const bool ini = name.size() > 4 && name.compare(name.size() - 4, 4, ".ini") == 0;
            if (ini) name.resize(name.size() - 4);
            if (name.empty() || name.size() > 9 || name.find_first_not_of("0123456789") != std::string::npos) continue;
            std::error_code e2;
            const DWORD attributes = GetFileAttributesW(p.c_str());   // review M-4: never follow a junction or link
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (ini ? !it->is_regular_file(e2) : !it->is_directory(e2)) continue;
            const DWORD pid = DWORD(std::stoul(name));
            if (pid == self || ProcessIsRunning(pid)) continue;
            fs::remove_all(p, e2);
        }
    } catch (...) {
    }
}

int Run(const Args& a)
{
    SweepStaleWorkDirs();
    namespace fs = std::filesystem;
    auto& emu = harness::Emulator::Instance();
    fs::path ini;
    int rc = kExitOk;
    try {
        const std::string pid = std::to_string(GetCurrentProcessId());
        fs::path dir = fs::temp_directory_path() / "applewin-harness";
        fs::create_directories(dir);
        ini = fs::absolute(dir / (pid + ".ini"));
        std::optional<harness::WorkingCopy> copy;   // destroyed after the try block, i.e. on every exit path
        harness::EmulatorConfig cfg;
        cfg.configIni = ini;
        cfg.display = a.headless ? harness::DisplayMode::Headless : harness::DisplayMode::Windowed;
        if (a.disk1) {
            copy.emplace(*a.disk1, fs::absolute(dir / pid));
            cfg.disk1 = copy->Path();
        }
        try {
            emu.Start(cfg);
            if (a.steps.empty()) {
                // Windowed, no steps: run until the window is closed.
                while (emu.RunRealtime(std::chrono::milliseconds(100))) {}
            } else if (!RunSteps(emu, a)) {
                std::cerr << "error: window closed\n";
                rc = kExitFailure;
            } else {
                harness::FrameView frame = emu.Frame();
                if (a.png && !harness::WritePngFile(*a.png, frame)) {
                    std::cerr << "error: cannot write " << a.png->string() << "\n";
                    rc = kExitFailure;
                } else {
                    std::cout << "sha256 " << harness::FrameHash(frame) << "\n";
                    if (emu.SoundActive())
                        std::cout << "audio underruns " << emu.SoundUnderruns() << "\n";
                    std::cout.flush();
                }
            }
        } catch (const std::exception& e) {
            ReleaseEmulator(emu);
            // Name the user's image, not the working copy.
            std::string what = e.what();
            if (copy && a.disk1) {
                const std::string from = copy->Path().string(), to = a.disk1->string();
                for (size_t pos = 0; (pos = what.find(from, pos)) != std::string::npos; pos += to.size())
                    what.replace(pos, from.size(), to);
            }
            throw std::runtime_error(what);
        } catch (...) {
            ReleaseEmulator(emu);
            throw;
        }
        ReleaseEmulator(emu);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        rc = kExitFailure;
    } catch (...) {
        std::cerr << "error: unknown exception" << std::endl;
        rc = kExitFailure;
    }
    std::error_code ec;
    if (!ini.empty()) fs::remove(ini, ec);
    return rc;
}

// A SYNCHRONIZE handle to the process that launched this one, or null when it is already gone or cannot be opened.
HANDLE OpenParentProcess()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    const DWORD self = GetCurrentProcessId();
    DWORD parentPid = 0;
    FILETIME selfCreated{}, parentCreated{}, unused{};
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (e.th32ProcessID == self) {
            parentPid = e.th32ParentProcessID;
            break;
        }
    CloseHandle(snap);
    if (parentPid == 0) return nullptr;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid);
    if (!h) return nullptr;
    // A recycled pid names an unrelated process: the real parent must have been created before this process.
    HANDLE me = GetCurrentProcess();
    if (!GetProcessTimes(me, &selfCreated, &unused, &unused, &unused) ||
        !GetProcessTimes(h, &parentCreated, &unused, &unused, &unused) ||
        CompareFileTime(&parentCreated, &selfCreated) > 0 || WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        CloseHandle(h);
        return nullptr;
    }
    return h;
}

// The JSON-RPC server (spec: docs/specs/m3.md, Part A "Launch"): announce, serve until close_session, the window
// closing (exit 0) or a fatal error (exit 1). The session removes the working copies and the INI on every path.
int Serve(const Args& a)
{
    SweepStaleWorkDirs();
    try {
        // Exit (0, normal cleanup) when the launching process exits. Opened before the announcement, so a launcher
        // that exits right after reading it is still seen; without a handle there is no watch.
        HANDLE parent = OpenParentProcess();
        if (!parent) std::cerr << "note: parent process not watched\n";
        const auto display = a.headless ? harness::DisplayMode::Headless : harness::DisplayMode::Windowed;
        const std::wstring pipe = harness::PipeServer::RandomPipeName();
        harness::Tokens tokens{harness::PipeServer::RandomToken(), harness::PipeServer::RandomToken()};
        const std::string agentToken = tokens.agent, operatorToken = tokens.operatorToken;
        // The session outlives the server: connection threads post to it until Stop has joined them.
        harness::Session session(display, harness::Emulator::Instance());
        session.SetParentProcess(parent);
        harness::PipeServer server(
            pipe, std::move(tokens),
            [&session](harness::ConnectionPtr c, nlohmann::json request) { session.Post(std::move(c), std::move(request)); },
            &harness::Session::IsOperatorOnly, display);
        server.Start();
        // The announcement: exactly one line, keys in the spec's order (ruling R7).
        nlohmann::ordered_json line;
        std::string pipeUtf8;   // the name is ASCII
        for (wchar_t ch : pipe) pipeUtf8.push_back(static_cast<char>(ch));
        line["pipe"] = pipeUtf8;
        line["agent_token"] = agentToken;
        line["operator_token"] = operatorToken;
        line["pid"] = GetCurrentProcessId();
        std::cout << line.dump() << "\n";
        std::cout.flush();
        session.SetFatalErrorSource([&server] { return server.FatalError(); });
        const int rc = session.RunMainLoop();
        if (parent) CloseHandle(parent);
        server.Stop();   // after the last replies (close_session's {}) have been written
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return kExitFailure;
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    Args a;
    if (!ParseArgs(argc, argv, a)) return Usage();
    return a.serve ? Serve(a) : Run(a);
}
