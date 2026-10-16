#include "host/emulator.h"
#include "core/png_writer.h"
#include "core/sha256.h"
#include "core/working_copy.h"
#include "server/pipe_server.h"
#include "server/protocol.h"
#include <nlohmann/json.hpp>
#include <windows.h>
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

int Run(const Args& a)
{
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

// The JSON-RPC server.
// TEMPORARY (M3A Task 4): transport only. Every authenticated request gets -32601 and the process serves until it is
// killed. Task 5 replaces this with the Session (create_session, close_session, the main loop and cleanup).
int Serve(const Args& a)
{
    try {
        const std::wstring pipe = harness::PipeServer::RandomPipeName();
        harness::Tokens tokens{harness::PipeServer::RandomToken(), harness::PipeServer::RandomToken()};
        const std::string agentToken = tokens.agent, operatorToken = tokens.operatorToken;
        auto isOperatorOnly = [](std::string_view m) {
            return m == "create_session" || m == "close_session" || m == "pause" || m == "resume" ||
                   m == "step_frames";
        };
        auto onRequest = [](harness::ConnectionPtr c, nlohmann::json request) {
            c->Send(harness::MakeError(request["id"], harness::kMethodNotFound, "method not found"));
        };
        harness::PipeServer server(pipe, std::move(tokens), onRequest, isOperatorOnly,
                                   a.headless ? harness::DisplayMode::Headless : harness::DisplayMode::Windowed);
        server.Start();
        // The announcement (spec: Part A "Launch"): exactly one line, keys in the spec's order (ruling R7).
        nlohmann::ordered_json line;
        std::string pipeUtf8;   // the name is ASCII
        for (wchar_t ch : pipe) pipeUtf8.push_back(static_cast<char>(ch));
        line["pipe"] = pipeUtf8;
        line["agent_token"] = agentToken;
        line["operator_token"] = operatorToken;
        line["pid"] = GetCurrentProcessId();
        std::cout << line.dump() << "\n";
        std::cout.flush();
        while (server.FatalError().empty()) Sleep(100);
        std::cerr << "error: " << server.FatalError() << "\n";
        return kExitFailure;
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
