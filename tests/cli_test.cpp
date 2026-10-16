#include "doctest/doctest.h"
#include <windows.h>
#include <filesystem>
#include <regex>
#include <string>

namespace {

struct CliResult {
    DWORD exitCode = 0xFFFFFFFF;
    std::string out;
    bool launched = false;
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
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) r.out.append(buf, got);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 120000);
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
    CHECK(r.exitCode == 0);
    CHECK(std::filesystem::exists(png));
    // stdout must be exactly the hash line (stderr is merged in the pipe, so nothing else may be printed).
    CHECK(std::regex_match(r.out, std::regex("sha256 [0-9a-f]{64}\r?\n")));
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
