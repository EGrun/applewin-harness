#include "doctest/doctest.h"
#include "host/emulator.h"
#include "core/sha256.h"
#include "support/emulator_fixture.h"
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace harness;
using namespace harness::testsupport;
using namespace std::chrono_literals;

namespace {

std::filesystem::path ExeDir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

// <exe dir>\..\..\..\tests\golden\<name>, first line trimmed; empty if missing.
std::string ReadTrimmed(const char* name)
{
    std::ifstream f(ExeDir() / ".." / ".." / ".." / "tests" / "golden" / name);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t b = s.find_first_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b);
}

}  // namespace

// doctest's default order is by file name, then line. No earlier file starts the emulator, so this case runs first.
TEST_CASE("emu: precondition")
{
    CHECK_THROWS_AS(Emulator::Instance().RunFrames(1), std::logic_error);
}

TEST_CASE("emu: start twice")
{
    auto& e = StartedEmulator();
    CHECK_THROWS_AS(e.Start({}), std::logic_error);
    CHECK(e.Messages().empty());
}

TEST_CASE("emu: boot text")
{
    auto& e = StartedEmulator();
    e.PowerCycle();
    e.RunFrames(120);
    CHECK(e.ReadTextRow(0).find("Apple //e") != std::string::npos);
}

TEST_CASE("emu: frame geometry")
{
    auto& e = StartedEmulator();
    auto v = e.Frame();
    CHECK(v.width == 560);
    CHECK(v.height == 384);
    CHECK(v.strideBytes == -600 * 4);
    auto b = e.Frame(true);
    CHECK(b.width == 600);
    CHECK(b.height == 420);
}

TEST_CASE("emu: frame-exact cycles")
{
    auto& e = StartedEmulator();
    auto c0 = e.EmulatedCycles();
    auto f0 = e.PresentedFrames();
    e.RunFrames(10);
    auto d = e.EmulatedCycles() - c0;
    CHECK(d >= 170'300);
    CHECK(d <= 170'300 + 7);
    auto df = e.PresentedFrames() - f0;
    CHECK(df >= 9);
    CHECK(df <= 11);
    auto c1 = e.EmulatedCycles();
    e.RunFrames(0);
    CHECK(e.EmulatedCycles() == c1);
}

TEST_CASE("emu: deterministic boot")
{
    auto& e = StartedEmulator();
    e.PowerCycle();
    e.RunFrames(120);
    auto a = FrameHash(e.Frame());
    e.PowerCycle();
    e.RunFrames(120);
    CHECK(FrameHash(e.Frame()) == a);
}

TEST_CASE("emu: golden boot")
{
    auto& e = StartedEmulator();
    e.PowerCycle();
    e.RunFrames(120);
    auto golden = ReadTrimmed("m1_boot_120.sha256");
    REQUIRE_MESSAGE(!golden.empty(), "run Task 7 to record the golden hash");
    CHECK(FrameHash(e.Frame()) == golden);
}

TEST_CASE("emu: no registry writes")
{
    auto& e = StartedEmulator();
    e.RunFrames(60);
    CHECK(ReadAppleWinRegistryStamp() == RegistryStampBeforeStart());   // {exists, last-write FILETIME}
}

TEST_SUITE("realtime")
{
    TEST_CASE("emu: realtime rate")
    {
        auto& e = StartedEmulator();
        auto c0 = e.EmulatedCycles();
        e.RunRealtime(2000ms);
        double d = double(e.EmulatedCycles() - c0);
        CHECK(d >= 2 * 1'020'484 * 0.98);
        CHECK(d <= 2 * 1'020'484 * 1.02);
    }
}
