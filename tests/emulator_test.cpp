#include "doctest/doctest.h"
#include "host/harness_frame.h"
#include "host/emulator.h"
#include "core/sha256.h"
#include "support/emulator_fixture.h"
#include "support/golden.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <windows.h>

#pragma warning(push, 3)
#include "Interface.h"
#include "Keyboard.h"
#pragma warning(pop)

using namespace harness;
using namespace harness::testsupport;
using namespace std::chrono_literals;

// "emu: precondition" lives at the top of dos_test.cpp: doctest orders by file name, and dos_test.cpp is the first file
// that starts the shared emulator.

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
    CHECK(b.strideBytes == -2400);
    // borderless topRow = bordered topRow moved down borderH (18) rows and right borderW (20) pixels
    CHECK(v.topRow == b.topRow + 18 * (-2400) + 20 * 4);
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
    auto golden = ReadGoldenTrimmed("m1_boot_120.sha256");
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

// ---- M2 additions (spec: docs/specs/m2.md, "src/host/emulator.h - changes") ----

TEST_CASE("emulator: EmulatedCycles is 0 straight after PowerCycle")
{
    auto& e = StartedEmulator();
    e.RunFrames(2);
    e.PowerCycle();
    CHECK(e.EmulatedCycles() == 0);
    e.RunFrames(1);
    const uint64_t d = e.EmulatedCycles();
    CHECK(d >= 17'030);
    CHECK(d <= 17'030 + 7);
}

TEST_CASE("emulator: TypeText rejects a control byte before typing anything")
{
    auto& e = StartedEmulator();
    e.PowerCycle();
    e.RunFrames(120);
    const BYTE before = KeybReadData();
    CHECK_THROWS_AS(e.TypeText("AB"), std::invalid_argument);
    CHECK_THROWS_AS(e.TypeText(""), std::invalid_argument);
    CHECK(KeybReadData() == before);
}

TEST_CASE("emulator: InsertDisk and EjectDisk reject drives other than 0 and 1")
{
    auto& e = StartedEmulator();
    CHECK_THROWS_AS(e.InsertDisk(2, "unused.dsk"), std::out_of_range);
    CHECK_THROWS_AS(e.InsertDisk(-1, "unused.dsk"), std::out_of_range);
    CHECK_THROWS_AS(e.EjectDisk(-1), std::out_of_range);
    CHECK_THROWS_AS(e.EjectDisk(2), std::out_of_range);
}

TEST_CASE("emulator: InsertDisk of a missing image throws runtime_error")
{
    auto& e = StartedEmulator();
    const auto missing = std::filesystem::temp_directory_path() / "applewin-harness" / "no-such-image.dsk";
    REQUIRE(!std::filesystem::exists(missing));
    CHECK_THROWS_AS(e.InsertDisk(0, missing), std::runtime_error);
}

TEST_CASE("emulator: headless start is silent")
{
    auto& e = StartedEmulator();
    CHECK(GetModuleHandleW(L"dsound.dll") == nullptr);
    CHECK(e.SoundActive() == false);
    CHECK(static_cast<HarnessFrame&>(GetFrame()).SoundBuffersCreated() == 0);
    CHECK(e.SoundUnderruns() == 0);
}

TEST_CASE("emulator: RunFrames returns true when no window was closed")
{
    auto& e = StartedEmulator();
    CHECK(e.RunFrames(5) == true);
}

TEST_CASE("emulator: Frame is the presented snapshot, not the live buffer")
{
    auto& e = StartedEmulator();
    e.RunFrames(2);
    const uint8_t* live = GetVideo().GetFrameBuffer();
    const size_t liveBytes = size_t(GetVideo().GetFrameBufferWidth()) * GetVideo().GetFrameBufferHeight() * 4;
    for (bool borders : {false, true}) {
        const uint8_t* top = e.Frame(borders).topRow;
        CHECK((top < live || top >= live + liveBytes));
    }
}
