#include "doctest/doctest.h"
#include "core/sha256.h"
#include "support/golden.h"
#include "support/png_reader.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CliResult {
    DWORD exitCode = 0xFFFFFFFF;
    std::string out;
    bool launched = false;
    bool timedOut = false;
};

std::filesystem::path ExeDir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

// Runs AppleWinHarness.exe (next to the test exe) with args, capturing stdout.
CliResult RunCli(const std::wstring& args)
{
    CliResult r;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    auto exe = ExeDir() / "AppleWinHarness.exe";
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             ExeDir().c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return r;
    }
    r.launched = true;
    // Drain the pipe on a helper thread so the wait below enforces the timeout even if the child hangs.
    std::string out;
    std::thread reader([&] {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    });
    if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) {
        r.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    reader.join();   // the pipe closes when the child exits (or is terminated)
    CloseHandle(rd);
    r.out = std::move(out);
    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

}  // namespace

TEST_CASE("cli: boot frames writes png and prints hash")
{
    auto tmp = std::filesystem::temp_directory_path() / "applewin-harness";
    std::filesystem::create_directories(tmp);
    auto png = tmp / "boot.png";
    std::filesystem::remove(png);

    auto r = RunCli(L"--boot-frames 120 --png \"" + png.wstring() + L"\"");
    REQUIRE(r.launched);
    REQUIRE_MESSAGE(!r.timedOut, "AppleWinHarness.exe did not finish within 120 s and was terminated");
    CHECK(r.exitCode == 0);
    REQUIRE(std::filesystem::exists(png));
    // stdout must be exactly the hash line (stderr is merged in the pipe, so nothing else may be printed).
    std::smatch m;
    REQUIRE(std::regex_match(r.out, m, std::regex("sha256 ([0-9a-f]{64})\r?\n")));

    const std::string golden = harness::testsupport::ReadGoldenTrimmed("m1_boot_120.sha256");
    REQUIRE_MESSAGE(!golden.empty(), "golden hash file missing");
    CHECK(m[1].str() == golden);

    // The artifact itself: 560x384 RGB whose pixel hash equals the golden.
    std::ifstream f(png, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto info = harness::testsupport::ReadPng(bytes);
    CHECK(info.allCrcsValid);
    REQUIRE(info.width == 560);
    REQUIRE(info.height == 384);
    std::vector<uint8_t> rgb;
    for (const auto& row : info.rgbRows) rgb.insert(rgb.end(), row.begin(), row.end());
    CHECK(harness::Sha256Hex(rgb) == golden);
}

TEST_CASE("cli: no arguments exits 2")
{
    auto r = RunCli(L"");
    REQUIRE(r.launched);
    CHECK(r.exitCode == 2);
}

TEST_CASE("cli: non-numeric boot-frames exits 2")
{
    auto r = RunCli(L"--boot-frames abc");
    REQUIRE(r.launched);
    CHECK(r.exitCode == 2);
}
