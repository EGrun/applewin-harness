#include "host/emulator.h"
#include "core/png_writer.h"
#include "core/sha256.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

struct Args {
    uint32_t bootFrames = 0;
    std::optional<std::filesystem::path> png;
};

int Usage()
{
    std::cerr << "usage: AppleWinHarness.exe --boot-frames <n> [--png <path>]\n";
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

bool ParseArgs(int argc, wchar_t** argv, Args& a)
{
    bool haveFrames = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring k = argv[i];
        if (k == L"--boot-frames" && i + 1 < argc && !haveFrames) {
            if (!ParseFrames(argv[++i], a.bootFrames)) return false;
            haveFrames = true;
        } else if (k == L"--png" && i + 1 < argc && !a.png) {
            a.png = std::filesystem::path(argv[++i]);
        } else {
            return false;
        }
    }
    return haveFrames;
}

int Run(const Args& a)
{
    namespace fs = std::filesystem;
    auto& emu = harness::Emulator::Instance();
    fs::path ini;
    int rc = kExitOk;
    try {
        fs::path dir = fs::temp_directory_path() / "applewin-harness";
        fs::create_directories(dir);
        ini = fs::absolute(dir / (std::to_string(GetCurrentProcessId()) + ".ini"));
        harness::EmulatorConfig cfg;
        cfg.configIni = ini;
        emu.Start(cfg);
        emu.RunFrames(a.bootFrames);
        harness::FrameView frame = emu.Frame();
        if (a.png && !harness::WritePngFile(*a.png, frame)) {
            std::cerr << "error: cannot write " << a.png->string() << "\n";
            rc = kExitFailure;
        } else {
            std::cout << "sha256 " << harness::FrameHash(frame) << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        rc = kExitFailure;
    }
    try { emu.Shutdown(); } catch (...) {}
    std::error_code ec;
    if (!ini.empty()) fs::remove(ini, ec);
    return rc;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    Args a;
    if (!ParseArgs(argc, argv, a)) return Usage();
    return Run(a);
}
