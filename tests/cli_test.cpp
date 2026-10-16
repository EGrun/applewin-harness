// AppleWinHarness.exe command-line tests (spec: docs/specs/m2.md, "src/main.cpp - command line" and "Test layout").
#include "doctest/doctest.h"
#include "core/sha256.h"
#include "support/golden.h"
#include "support/paths.h"
#include "support/png_reader.h"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using harness::testsupport::DosMasterImage;
using harness::testsupport::ReadGoldenTrimmed;

namespace {

struct CliResult {
    DWORD exitCode = 0xFFFFFFFF;
    std::string out;   // stdout, with stderr merged in unless the process was launched with mergeStderr = false
    std::string err;   // stderr when launched with mergeStderr = false
    bool launched = false;
    bool timedOut = false;
};

std::filesystem::path ExeDir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

// A running AppleWinHarness.exe (next to the test exe) whose output is drained on helper threads. The destructor
// terminates the child if Wait was never called, so failure paths never leak a process.
class CliProcess {
public:
    CliProcess(const std::wstring& args, bool mergeStderr)
    {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE outWr = nullptr, errWr = nullptr;
        if (!CreatePipe(&outRd_, &outWr, &sa, 0)) return;
        SetHandleInformation(outRd_, HANDLE_FLAG_INHERIT, 0);
        if (!mergeStderr) {
            if (!CreatePipe(&errRd_, &errWr, &sa, 0)) {
                CloseHandle(outWr);
                return;
            }
            SetHandleInformation(errRd_, HANDLE_FLAG_INHERIT, 0);
        }

        const auto exe = ExeDir() / "AppleWinHarness.exe";
        std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = outWr;
        si.hStdError = mergeStderr ? outWr : errWr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       ExeDir().c_str(), &si, &pi_);
        CloseHandle(outWr);
        if (errWr) CloseHandle(errWr);
        if (!ok) {
            pi_ = {};
            return;
        }
        // The pipes close when the child exits (or is terminated), which ends the readers.
        outReader_ = std::thread([this] { Drain(outRd_, out_); });
        if (errRd_) errReader_ = std::thread([this] { Drain(errRd_, err_); });
    }

    ~CliProcess()
    {
        if (pi_.hProcess && !waited_) {
            TerminateProcess(pi_.hProcess, 1);
            WaitForSingleObject(pi_.hProcess, 10000);
        }
        JoinReaders();
        if (outRd_) CloseHandle(outRd_);
        if (errRd_) CloseHandle(errRd_);
        if (pi_.hProcess) CloseHandle(pi_.hProcess);
        if (pi_.hThread) CloseHandle(pi_.hThread);
    }

    CliProcess(const CliProcess&) = delete;
    CliProcess& operator=(const CliProcess&) = delete;

    bool Launched() const { return pi_.hProcess != nullptr; }
    DWORD Pid() const { return pi_.dwProcessId; }
    HANDLE Process() const { return pi_.hProcess; }
    bool Running() const { return pi_.hProcess && WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT; }

    // Waits for the child to exit; terminates it after timeoutMs.
    CliResult Wait(DWORD timeoutMs)
    {
        CliResult r;
        r.launched = Launched();
        if (!r.launched) return r;
        if (WaitForSingleObject(pi_.hProcess, timeoutMs) != WAIT_OBJECT_0) {
            r.timedOut = true;
            TerminateProcess(pi_.hProcess, 1);
            WaitForSingleObject(pi_.hProcess, 10000);
        }
        waited_ = true;
        JoinReaders();
        GetExitCodeProcess(pi_.hProcess, &r.exitCode);
        r.out = out_;
        r.err = err_;
        return r;
    }

private:
    static void Drain(HANDLE rd, std::string& sink)
    {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) sink.append(buf, got);
    }

    void JoinReaders()
    {
        if (outReader_.joinable()) outReader_.join();
        if (errReader_.joinable()) errReader_.join();
    }

    PROCESS_INFORMATION pi_{};
    HANDLE outRd_ = nullptr, errRd_ = nullptr;
    std::thread outReader_, errReader_;
    std::string out_, err_;
    bool waited_ = false;
};

// Starts AppleWinHarness.exe without waiting for it.
std::unique_ptr<CliProcess> LaunchCli(const std::wstring& args, bool mergeStderr = true)
{
    return std::make_unique<CliProcess>(args, mergeStderr);
}

// Runs AppleWinHarness.exe with args and waits up to timeoutMs (then terminates it). stderr is merged into stdout.
CliResult RunCli(const std::wstring& args, DWORD timeoutMs = 120000)
{
    return LaunchCli(args)->Wait(timeoutMs);
}

// The top-level window of class AppleWinHarnessWindow owned by process pid, or nullptr.
HWND FindHarnessWindow(DWORD pid)
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

// Polls for the child's visible harness window; gives up early if the child exits.
HWND WaitForHarnessWindow(const CliProcess& p, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        HWND h = FindHarnessWindow(p.Pid());
        if (h && IsWindowVisible(h)) return h;
        if (!p.Running()) return nullptr;
        std::this_thread::sleep_for(50ms);
    }
    return nullptr;
}

std::wstring Quoted(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

fs::path HarnessTempDir() { return fs::temp_directory_path() / "applewin-harness"; }

// A file path in %TEMP%\applewin-harness that is removed when the guard ends.
struct TempFile {
    fs::path path;
    explicit TempFile(const std::string& name) : path(HarnessTempDir() / name)
    {
        fs::create_directories(path.parent_path());
        std::error_code ec;
        fs::remove(path, ec);
    }
    ~TempFile()
    {
        std::error_code ec;
        fs::remove(path, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

std::vector<uint8_t> ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The 64-hex hash of the first "sha256 <hash>" line in text, or "".
std::string HashIn(const std::string& text)
{
    std::smatch m;
    return std::regex_search(text, m, std::regex("sha256 ([0-9a-f]{64})")) ? m[1].str() : std::string();
}

std::string FirstLine(const std::string& text)
{
    std::string line = text.substr(0, text.find('\n'));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

// Checks that a 560x384 PNG's RGB pixels hash to expected.
void CheckPng(const fs::path& png, const std::string& expected)
{
    REQUIRE(fs::exists(png));
    auto info = harness::testsupport::ReadPng(ReadAll(png));
    CHECK(info.allCrcsValid);
    REQUIRE(info.width == 560);
    REQUIRE(info.height == 384);
    std::vector<uint8_t> rgb;
    for (const auto& row : info.rgbRows) rgb.insert(rgb.end(), row.begin(), row.end());
    CHECK(harness::Sha256Hex(rgb) == expected);
}

const std::wstring kDosPrintSteps = L" --frames 720 --type \"PRINT 2+2\\r\" --frames 60";

}  // namespace

// ---- M1 tests, switched to the M2 command line ----

TEST_CASE("cli: headless frames writes png and prints hash")
{
    TempFile png("boot.png");
    auto r = RunCli(L"--headless --frames 120 --png " + Quoted(png.path));
    REQUIRE(r.launched);
    REQUIRE_MESSAGE(!r.timedOut, "AppleWinHarness.exe did not finish within 120 s and was terminated");
    CHECK(r.exitCode == 0);
    // stdout must be exactly the hash line (stderr is merged in the pipe, so nothing else may be printed).
    std::smatch m;
    REQUIRE(std::regex_match(r.out, m, std::regex("sha256 ([0-9a-f]{64})\r?\n")));
    const std::string golden = ReadGoldenTrimmed("m1_boot_120.sha256");
    REQUIRE_MESSAGE(!golden.empty(), "golden hash file missing");
    CHECK(m[1].str() == golden);
    CheckPng(png.path, golden);
}

TEST_CASE("cli: headless with no steps exits 2")
{
    auto r = RunCli(L"--headless");
    REQUIRE(r.launched);
    CHECK(r.exitCode == 2);
}

TEST_CASE("cli: non-numeric frames exits 2")
{
    auto r = RunCli(L"--headless --frames abc");
    REQUIRE(r.launched);
    CHECK(r.exitCode == 2);
}

// ---- M2 headless tests ----

TEST_CASE("cli: headless DOS golden leaves the original image untouched")
{
    const fs::path dos = DosMasterImage();
    REQUIRE(fs::exists(dos));
    const std::string shaBefore = harness::Sha256Hex(ReadAll(dos));
    const auto mtimeBefore = fs::last_write_time(dos);

    TempFile png("dos_print.png");
    auto r = RunCli(L"--headless --disk1 " + Quoted(dos) + kDosPrintSteps + L" --png " + Quoted(png.path));
    REQUIRE(r.launched);
    REQUIRE_MESSAGE(!r.timedOut, "AppleWinHarness.exe did not finish within 120 s and was terminated");
    CHECK(r.exitCode == 0);

    const std::string golden = ReadGoldenTrimmed("m2_dos_print.sha256");
    REQUIRE_MESSAGE(!golden.empty(), "golden m2_dos_print.sha256 missing");
    // Exactly the hash line: nothing else on stdout or (merged) stderr.
    CHECK(std::regex_match(r.out, std::regex("sha256 " + golden + "\r?\n")));
    CheckPng(png.path, golden);

    CHECK(harness::Sha256Hex(ReadAll(dos)) == shaBefore);
    CHECK(fs::last_write_time(dos) == mtimeBefore);
}

TEST_CASE("cli: usage errors exit 2")
{
    const std::wstring dos = Quoted(DosMasterImage());
    const std::wstring cases[] = {
        L"--headless",
        L"--headless --type \"\"",
        L"--headless --type \"a\\qb\"",
        L"--headless --frames abc",
        L"--headless --disk1 " + dos + L" --disk1 " + dos + L" --frames 1",
        L"--headless --boot-frames 120",
    };
    for (const auto& args : cases) {
        INFO("args: " << fs::path(args).string());
        auto r = RunCli(args);
        REQUIRE(r.launched);
        CHECK(r.exitCode == 2);
    }
}

TEST_CASE("cli: a missing disk1 file exits 1")
{
    const fs::path missing = HarnessTempDir() / "no-such-image.dsk";
    REQUIRE(!fs::exists(missing));
    auto r = RunCli(L"--headless --disk1 " + Quoted(missing) + L" --frames 1");
    REQUIRE(r.launched);
    CHECK(r.exitCode == 1);
}

// A 1000-byte file named bad.dsk (an unsupported image size) in a fresh temp directory removed by the guard.
struct BadImage {
    fs::path dir, path;
    BadImage()
    {
        dir = fs::temp_directory_path() / ("applewin-harness-bad-" + std::to_string(GetCurrentProcessId()) + "-" +
                                            std::to_string(GetTickCount64()));
        fs::create_directories(dir);
        path = dir / "bad.dsk";
        std::ofstream f(path, std::ios::binary);
        const std::string data(1000, 'x');
        f.write(data.data(), std::streamsize(data.size()));
    }
    ~BadImage()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    BadImage(const BadImage&) = delete;
    BadImage& operator=(const BadImage&) = delete;
};

// Start fails while inserting disk1 (Start shuts down, the Start guard releases again, main shuts down a third time):
// exit 1, a readable error naming the user's image, no leftovers, no assert dialog (the child exits in time).
void CheckInvalidDisk1Cleanup(const std::wstring& mode)
{
    BadImage bad;
    auto p = LaunchCli(mode + L"--disk1 " + Quoted(bad.path) + L" --frames 1");
    REQUIRE(p->Launched());
    const DWORD pid = p->Pid();
    auto r = p->Wait(60000);
    REQUIRE(!r.timedOut);
    INFO("output: " << r.out);
    CHECK(r.exitCode == 1);
    CHECK(r.out.find("cannot insert") != std::string::npos);
    CHECK(r.out.find(bad.path.string()) != std::string::npos);   // the original path, not the working copy
    CHECK(!fs::exists(HarnessTempDir() / std::to_string(pid)));
    CHECK(!fs::exists(HarnessTempDir() / (std::to_string(pid) + ".ini")));
    CHECK(FindHarnessWindow(pid) == nullptr);
}

TEST_CASE("cli: an invalid disk1 image exits 1 and cleans up")
{
    CheckInvalidDisk1Cleanup(L"--headless ");
}

TEST_CASE("cli: the working copy and INI are gone after a run")
{
    auto p = LaunchCli(L"--headless --disk1 " + Quoted(DosMasterImage()) + L" --frames 1");
    REQUIRE(p->Launched());
    const DWORD pid = p->Pid();
    auto r = p->Wait(120000);
    REQUIRE(!r.timedOut);
    CHECK(r.exitCode == 0);
    CHECK(!fs::exists(HarnessTempDir() / std::to_string(pid)));
    CHECK(!fs::exists(HarnessTempDir() / (std::to_string(pid) + ".ini")));
}

// ---- Windowed: need a desktop session (pass conditions 2, 5 and 6) ----

TEST_SUITE("windowed")
{
    TEST_CASE("cli: windowed, an invalid disk1 image exits 1 and cleans up")
    {
        CheckInvalidDisk1Cleanup(L"");
    }

    TEST_CASE("cli: windowed DOS golden prints the same hash")
    {
        auto p = LaunchCli(L"--disk1 " + Quoted(DosMasterImage()) + kDosPrintSteps, /*mergeStderr*/ false);
        REQUIRE(p->Launched());
        auto r = p->Wait(120000);
        REQUIRE(!r.timedOut);
        CHECK(r.exitCode == 0);
        const std::string golden = ReadGoldenTrimmed("m2_dos_print.sha256");
        REQUIRE_MESSAGE(!golden.empty(), "golden m2_dos_print.sha256 missing");
        INFO("stdout: " << r.out << "stderr: " << r.err);
        CHECK(FirstLine(r.out) == "sha256 " + golden);
    }

    TEST_CASE("cli: no arguments opens the window and WM_CLOSE exits 0")
    {
        auto p = LaunchCli(L"", /*mergeStderr*/ false);
        REQUIRE(p->Launched());
        HWND h = WaitForHarnessWindow(*p, 10s);
        REQUIRE_MESSAGE(h != nullptr, "no visible AppleWinHarnessWindow owned by the child within 10 s");
        REQUIRE(PostMessageW(h, WM_CLOSE, 0, 0));
        auto r = p->Wait(30000);
        REQUIRE(!r.timedOut);
        CHECK(r.exitCode == 0);
        CHECK(r.out.empty());
    }

    TEST_CASE("cli: keys and clicks sent to the window don't change the hash")
    {
        const std::wstring disk = L"--disk1 " + Quoted(DosMasterImage()) + L" --frames 720";
        auto p = LaunchCli(L"--realtime " + disk, /*mergeStderr*/ false);
        REQUIRE(p->Launched());
        const auto t0 = std::chrono::steady_clock::now();
        HWND h = WaitForHarnessWindow(*p, 10s);
        REQUIRE_MESSAGE(h != nullptr, "no visible AppleWinHarnessWindow owned by the child within 10 s");

        // "HELLO\r" as WM_CHARs, each with WM_KEYDOWN/WM_KEYUP and a left click, spread from 3 s to 6 s after launch.
        const std::string keys = "HELLO\r";
        for (size_t i = 0; i < keys.size(); ++i) {
            std::this_thread::sleep_until(t0 + 3s + std::chrono::milliseconds(500 * int(i)));
            const char c = keys[i];
            const WPARAM vk = c == '\r' ? VK_RETURN : WPARAM(VkKeyScanA(c) & 0xFF);
            PostMessageW(h, WM_KEYDOWN, vk, 1);
            PostMessageW(h, WM_CHAR, WPARAM(c), 1);
            PostMessageW(h, WM_KEYUP, vk, LPARAM(0xC0000001));
            PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
            PostMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(100, 100));
        }
        std::this_thread::sleep_until(t0 + 6s);

        auto r = p->Wait(120000);
        REQUIRE(!r.timedOut);
        CHECK(r.exitCode == 0);
        const std::string withInput = HashIn(r.out);
        REQUIRE_MESSAGE(!withInput.empty(), "no hash line: stdout " << r.out << " stderr " << r.err);

        auto quiet = RunCli(L"--headless " + disk);
        REQUIRE(!quiet.timedOut);
        CHECK(quiet.exitCode == 0);
        CHECK(HashIn(quiet.out) == withInput);
    }

    TEST_CASE("cli: closing the window during the steps exits 1")
    {
        auto p = LaunchCli(L"--realtime --disk1 " + Quoted(DosMasterImage()) + L" --frames 3000", /*mergeStderr*/ false);
        REQUIRE(p->Launched());
        const DWORD pid = p->Pid();
        HWND h = WaitForHarnessWindow(*p, 10s);
        REQUIRE_MESSAGE(h != nullptr, "no visible AppleWinHarnessWindow owned by the child within 10 s");
        std::this_thread::sleep_for(2s);
        REQUIRE(PostMessageW(h, WM_CLOSE, 0, 0));
        auto r = p->Wait(60000);
        REQUIRE(!r.timedOut);
        CHECK(r.exitCode == 1);
        CHECK(r.err.find("error: window closed") != std::string::npos);
        CHECK(!fs::exists(HarnessTempDir() / std::to_string(pid)));
        CHECK(!fs::exists(HarnessTempDir() / (std::to_string(pid) + ".ini")));
    }
}

// ---- Audio: need an audio device (pass condition 8) ----

TEST_SUITE("audio")
{
    TEST_CASE("cli: 60-second speaker tone has no underruns")
    {
        // Boot DOS, then a BASIC loop that toggles the speaker ($C030) forever, then 3,600 realtime frames (~60 s).
        auto p = LaunchCli(L"--realtime --disk1 " + Quoted(DosMasterImage()) +
                               L" --frames 720 --type \"10 X=PEEK(-16336):GOTO 10\\rRUN\\r\" --frames 3600",
                           /*mergeStderr*/ false);
        REQUIRE(p->Launched());
        auto r = p->Wait(240000);
        REQUIRE(!r.timedOut);
        if ((r.out + r.err).find("no audio device") != std::string::npos) {
            MESSAGE("skipped: the harness reported no audio device");
            return;
        }
        CHECK(r.exitCode == 0);
        std::smatch m;
        REQUIRE_MESSAGE(std::regex_search(r.out, m, std::regex("audio underruns ([0-9]+)")),
                        "no audio underruns line: stdout " << r.out << " stderr " << r.err);
        CHECK(m[1].str() == "0");
    }
}
