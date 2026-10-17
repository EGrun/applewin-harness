// M3 Part C (D11): speed controls. Spec: docs/specs/m3.md Part C, docs/api_spec.md "Speed".
// The in-process cases ("speed: ...") are fast and deterministic. The rate and full-speed cases are in suite
// "realtime" (headless, wall-clock timing).
#include "doctest/doctest.h"
#include "host/emulator.h"
#include "core/audio_capture.h"
#include "core/sha256.h"
#include "core/working_copy.h"
#include "support/emulator_fixture.h"
#include "support/paths.h"

#pragma warning(push, 3)
#include "Common.h"
#include "Core.h"
#include "Memory.h"
#include "Interface.h"
#pragma warning(pop)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

using namespace harness;
using namespace harness::testsupport;
using namespace std::chrono_literals;

namespace {

// Back to the M1 state however a test ends: authentic speed, no override, enhanced disk speed on. Never throws.
struct RestoreSpeed {
    Emulator& e;
    ~RestoreSpeed()
    {
        try { e.SetFullSpeedOverride(false); } catch (...) {}
        try { e.SetEnhanceDiskSpeed(true); } catch (...) {}
        try { e.SetMachineSpeed(SPEED_NORMAL); } catch (...) {}
        g_dwSpeed = SPEED_NORMAL;   // AppleWin's applied clock too, for tests that set it directly
        SetCurrentCLK6502();
    }
};

// Ejects the disk and power-cycles, so later tests start from the M1 state. Never throws.
struct EjectAndPowerCycle {
    Emulator& e;
    ~EjectAndPowerCycle()
    {
        try { e.EjectDisk(0); } catch (...) {}
        try { e.PowerCycle(); } catch (...) {}
    }
};

// The fixture boots with an empty drive, so the Disk II boot ROM keeps the motor on; with enhanced disk speed that is
// AppleWin's disk_motor full-speed condition. Rate tests turn enhancement off (RestoreSpeed turns it back on).
void NoMotorFullSpeed(Emulator& e)
{
    e.SetEnhanceDiskSpeed(false);
    REQUIRE(e.FullSpeedReasons().empty());
}

bool Contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Emulated cycles per wall second over `wall`, after `settle` of wall time at the current settings.
double MeasureRate(Emulator& e, std::chrono::milliseconds settle, std::chrono::milliseconds wall)
{
    if (settle.count() > 0) REQUIRE(e.RunRealtime(settle));
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = e.EmulatedCycles();
    REQUIRE(e.RunRealtime(wall));
    const uint64_t c1 = e.EmulatedCycles();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return double(c1 - c0) / secs;
}

// True once a text row starts with the "]" Applesoft prompt.
bool AtPrompt(const Emulator& e)
{
    for (int r = 0; r < 24; ++r) {
        const std::string row = e.ReadTextRow(r);
        if (!row.empty() && row[0] == ']') return true;
    }
    return false;
}

// "machine_speed_max", "override", "disk_motor": the order get_speed reports (a subsequence of it).
bool InCanonicalOrder(const std::vector<std::string>& reasons)
{
    static const std::vector<std::string> order = {"machine_speed_max", "override", "disk_motor"};
    size_t at = 0;
    for (const std::string& r : reasons) {
        while (at < order.size() && order[at] != r) ++at;
        if (at == order.size()) return false;
        ++at;
    }
    return true;
}

// PowerCycle must keep the speed settings (they belong to the session), so the speed is checked after it and at the end.
std::string BootAndPrintHash(Emulator& e, uint32_t expectedSpeed)
{
    e.PowerCycle();
    CHECK(e.Speed().machineSpeed == expectedSpeed);
    REQUIRE(e.RunFrames(720));
    REQUIRE(e.TypeText("PRINT 2+2\r"));
    REQUIRE(e.RunFrames(60));
    CHECK(e.Speed().machineSpeed == expectedSpeed);
    return FrameHash(e.Frame());
}

std::string AudioHash(const std::vector<int16_t>& pcm)
{
    return Sha256Hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t)));
}

}  // namespace

TEST_CASE("speed: defaults are authentic speed, enhanced disk, no override")
{
    auto& e = StartedEmulator();
    RestoreSpeed restore{e};
    const SpeedSettings s = e.Speed();
    CHECK(s.machineSpeed == 10);
    CHECK(s.enhanceDisk);
    CHECK_FALSE(s.fullSpeedOverride);
    CHECK(e.EffectiveClockHz() == doctest::Approx(CLK_6502_NTSC).epsilon(1e-9));
    // The fixture's empty drive may have the boot ROM's motor on (disk_motor with enhance on), depending on what ran
    // before; with enhance off no reason is left.
    NoMotorFullSpeed(e);
    CHECK_FALSE(e.FullSpeedActive());
}

TEST_CASE("speed: SetMachineSpeed 5, 10 and 20 give AppleWin's slider clock")
{
    auto& e = StartedEmulator();
    RestoreSpeed restore{e};
    const struct { uint32_t speed; double factor; } cases[] = {{5, 0.75}, {10, 1.0}, {20, 2.0}};
    for (const auto& c : cases) {
        CAPTURE(c.speed);
        e.SetMachineSpeed(c.speed);
        CHECK(e.Speed().machineSpeed == c.speed);
        CHECK(e.EffectiveClockHz() == doctest::Approx(CLK_6502_NTSC * c.factor).epsilon(1e-9));
        // The clock AppleWin itself computes for that speed (it is applied at the next realtime step).
        g_dwSpeed = c.speed;
        SetCurrentCLK6502();
        CHECK(g_fCurrentCLK6502 == doctest::Approx(e.EffectiveClockHz()).epsilon(1e-12));
    }
}

TEST_CASE("speed: SetMachineSpeed rejects values above 40 and keeps the old speed")
{
    auto& e = StartedEmulator();
    RestoreSpeed restore{e};
    e.SetMachineSpeed(20);
    CHECK_THROWS_AS(e.SetMachineSpeed(41), std::out_of_range);
    CHECK(e.Speed().machineSpeed == 20);
    e.SetMachineSpeed(0);   // the bounds themselves are valid
    CHECK(e.Speed().machineSpeed == 0);
    e.SetMachineSpeed(40);
    CHECK(e.Speed().machineSpeed == 40);
}

TEST_CASE("speed: UseClockMultiplier gives the g_dwSpeed AppleWin's own function gives (Review Focus 5)")
{
    auto& e = StartedEmulator();
    RestoreSpeed restore{e};
    for (const double x : {0.3, 0.5, 0.75, 1.0, 1.55, 2.0, 3.95, 10.0}) {
        CAPTURE(x);
        g_dwSpeed = SPEED_NORMAL;
        SetCurrentCLK6502();
        ::UseClockMultiplier(x);
        const uint32_t expected = g_dwSpeed;
        const double expectedClock = g_fCurrentCLK6502;
        CHECK(expected < SPEED_MAX);   // never selects unthrottled speed

        g_dwSpeed = SPEED_NORMAL;
        SetCurrentCLK6502();
        e.UseClockMultiplier(x);
        CHECK(e.Speed().machineSpeed == expected);
        CHECK(e.EffectiveClockHz() == doctest::Approx(expectedClock).epsilon(1e-12));
    }
    // Huge values are clamped before AppleWin's (ULONG)(x * 10) cast, which is undefined above about 4.29e8; the
    // result is still AppleWin's cap, 39.
    for (const double x : {4.0, 1e9, 1e30, (std::numeric_limits<double>::max)()}) {
        CAPTURE(x);
        e.SetMachineSpeed(10);
        e.UseClockMultiplier(x);
        CHECK(e.Speed().machineSpeed == 39);
    }
}

TEST_CASE("speed: UseClockMultiplier rejects values that are not finite and above 0")
{
    auto& e = StartedEmulator();
    RestoreSpeed restore{e};
    e.SetMachineSpeed(15);
    for (const double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        CAPTURE(bad);
        CHECK_THROWS_AS(e.UseClockMultiplier(bad), std::invalid_argument);
    }
    CHECK(e.Speed().machineSpeed == 15);
}

TEST_CASE("speed: enhance disk speed and the override are stored")
{
    auto& e = StartedEmulator();
    RestoreSpeed restore{e};
    e.SetEnhanceDiskSpeed(false);
    e.SetFullSpeedOverride(true);
    CHECK_FALSE(e.Speed().enhanceDisk);
    CHECK(e.Speed().fullSpeedOverride);
    e.SetEnhanceDiskSpeed(true);
    e.SetFullSpeedOverride(false);
    CHECK(e.Speed().enhanceDisk);
    CHECK_FALSE(e.Speed().fullSpeedOverride);
}

TEST_CASE("speed: FullSpeedReasons lists machine_speed_max, then override")
{
    auto& e = StartedEmulator();
    RestoreSpeed restore{e};
    using V = std::vector<std::string>;
    NoMotorFullSpeed(e);   // order-independent: the fixture's drive motor may be on (RestoreSpeed turns enhance back on)
    e.SetFullSpeedOverride(true);
    CHECK(e.FullSpeedReasons() == V{"override"});
    CHECK(e.FullSpeedActive());
    e.SetMachineSpeed(40);
    CHECK(e.FullSpeedReasons() == V{"machine_speed_max", "override"});
    e.SetFullSpeedOverride(false);
    CHECK(e.FullSpeedReasons() == V{"machine_speed_max"});
    CHECK(e.FullSpeedActive());
    e.SetMachineSpeed(39);
    CHECK(e.FullSpeedReasons().empty());
    CHECK_FALSE(e.FullSpeedActive());
}

TEST_CASE("speed: stepped RunFrames gives the same hash at speeds 5 and 40 and with the override (Review Focus 1)")
{
    auto& e = StartedEmulator();
    RestoreSpeed restore{e};
    const auto work = std::filesystem::temp_directory_path() / ("awh-speed-hash-" + std::to_string(GetCurrentProcessId()));
    WorkingCopy copy(DosMasterImage(), work);
    e.InsertDisk(0, copy.Path());
    EjectAndPowerCycle ejector{e};   // declared after copy: ejects before the copy is deleted

    e.SetMachineSpeed(5);
    const std::string at5 = BootAndPrintHash(e, 5);
    const uint64_t cycles5 = e.EmulatedCycles();
    e.SetMachineSpeed(40);
    const std::string at40 = BootAndPrintHash(e, 40);
    const uint64_t cycles40 = e.EmulatedCycles();
    e.SetMachineSpeed(10);
    e.SetFullSpeedOverride(true);
    const std::string withOverride = BootAndPrintHash(e, 10);
    CHECK(at5 == at40);
    CHECK(at5 == withOverride);
    CHECK(cycles5 == cycles40);
}

// Ruling C7: stepped runs use AppleWin's defaults (speed 10, enhanced disk access) whatever the session's settings, so
// no speed setting changes stepped results: frames sampled every 10 frames during a DOS boot (where enhance_disk_speed
// would change disk timing), main RAM at the same points, and the captured audio of the same script (where
// machine_speed would change AppleWin's samples per cycle). The script is many separate stepped runs on purpose, and
// one configuration changes the speed in the middle of it.
TEST_CASE("speed: stepped runs give the same frames, RAM and audio at every speed setting (Ruling C7)")
{
    auto& e = StartedEmulator();
    RestoreSpeed restore{e};
    const auto work = std::filesystem::temp_directory_path() / ("awh-speed-c7-" + std::to_string(GetCurrentProcessId()));
    WorkingCopy copy(DosMasterImage(), work);
    e.InsertDisk(0, copy.Path());
    EjectAndPowerCycle ejector{e};

    struct Config { uint32_t speed; bool enhance; uint32_t midSpeed; };   // midSpeed: set after frame 360 (0: none)
    const Config configs[] = {
        {10, true, 0},    // warm-up, not compared: the previous tests leave state that PowerCycle does not reset
        {10, true, 0},    // the reference
        {5, true, 0}, {20, true, 0}, {40, true, 0}, {10, false, 0}, {20, false, 0}, {20, true, 5}, {10, true, 0}};
    struct Result { std::vector<std::string> frames, ram; std::vector<int16_t> audio; uint64_t cycles = 0; };
    std::vector<Result> results;
    for (const Config& c : configs) {
        CAPTURE(c.speed);
        CAPTURE(c.enhance);
        CAPTURE(c.midSpeed);
        e.SetMachineSpeed(c.speed);
        e.SetEnhanceDiskSpeed(c.enhance);
        // The video scanner's position within the frame survives a power cycle (in AppleWin too). Start every boot at
        // the top of a frame, the way AppleWin restores it from a save-state, so the sampled frames are comparable.
        g_dwCyclesThisFrame = 0;
        GetVideo().VideoReinitialize(true);
        e.PowerCycle();
        Result r;
        uint32_t speed = c.speed;
        for (int i = 0; i < 72; ++i) {   // the DOS boot, 10 frames per run
            REQUIRE(e.RunFrames(10));
            r.frames.push_back(FrameHash(e.Frame()));
            r.ram.push_back(Sha256Hex(std::span<const uint8_t>(MemGetMainPtr(0), 0xC000)));
            // The session's settings are unchanged after every stepped run.
            REQUIRE(e.Speed().machineSpeed == speed);
            REQUIRE(e.Speed().enhanceDisk == c.enhance);
            if (i == 35 && c.midSpeed != 0) {
                e.SetMachineSpeed(c.midSpeed);
                speed = c.midSpeed;
            }
        }
        REQUIRE(e.TypeText("PRINT CHR$(7)\r"));
        REQUIRE(e.RunFrames(60));
        r.frames.push_back(FrameHash(e.Frame()));
        r.cycles = e.EmulatedCycles();
        r.audio = e.Audio().Read(0, r.cycles);
        CHECK(e.Speed().machineSpeed == speed);
        CHECK(e.Speed().enhanceDisk == c.enhance);
        results.push_back(std::move(r));
    }
    // The bell really is in the audio (otherwise the comparison would be vacuous).
    const Result& ref = results[1];
    REQUIRE(!ref.audio.empty());
    CHECK(std::any_of(ref.audio.begin(), ref.audio.end(), [&](int16_t v) { return v != ref.audio.front(); }));
    for (size_t i = 2; i < results.size(); ++i) {
        CAPTURE(configs[i].speed);
        CAPTURE(configs[i].enhance);
        CAPTURE(configs[i].midSpeed);
        CHECK(results[i].cycles == ref.cycles);
        std::string differingFrames, differingRam;   // the sample points (in frames) whose hash differs
        for (size_t f = 0; f < ref.frames.size(); ++f) {
            const std::string at = std::to_string(f < 72 ? (f + 1) * 10 : 720 + 60) + " ";
            if (results[i].frames[f] != ref.frames[f]) differingFrames += at;
            if (f < ref.ram.size() && results[i].ram[f] != ref.ram[f]) differingRam += at;
        }
        CAPTURE(differingFrames);
        CAPTURE(differingRam);
        CHECK(differingFrames.empty());
        CHECK(differingRam.empty());
        CHECK(results[i].audio.size() == ref.audio.size());
        CHECK(AudioHash(results[i].audio) == AudioHash(ref.audio));
    }
}

TEST_SUITE("realtime")
{
    TEST_CASE("speed: the rate at machine_speed 5, 10 and 20 is within 2% (3% on CI) of EffectiveClockHz (pass condition 10)")
    {
        auto& e = StartedEmulator();
        RestoreSpeed restore{e};
        NoMotorFullSpeed(e);
        for (const uint32_t speed : {5u, 10u, 20u}) {
            CAPTURE(speed);
            e.SetMachineSpeed(speed);
            const double hz = e.EffectiveClockHz();
            const double rate = MeasureRate(e, 1000ms, 3000ms);
            CAPTURE(rate);
            CAPTURE(hz);
            CHECK(rate >= hz * (1 - RealtimeRateTolerance()));
            CHECK(rate <= hz * (1 + RealtimeRateTolerance()));
        }
    }

    TEST_CASE("speed: max and the override each run over 2x authentic (pass condition 11)")
    {
        auto& e = StartedEmulator();
        RestoreSpeed restore{e};
        NoMotorFullSpeed(e);
        e.SetMachineSpeed(40);
        CHECK(e.FullSpeedActive());
        const double max = MeasureRate(e, 1000ms, 3000ms);
        CAPTURE(max);
        CHECK(max > 2.0 * CLK_6502_NTSC);

        e.SetMachineSpeed(10);
        CHECK_FALSE(e.FullSpeedActive());
        e.SetFullSpeedOverride(true);
        CHECK(e.FullSpeedActive());
        const double over = MeasureRate(e, 1000ms, 3000ms);
        CAPTURE(over);
        CHECK(over > 2.0 * CLK_6502_NTSC);

        e.SetFullSpeedOverride(false);
        const double back = MeasureRate(e, 1000ms, 3000ms);
        CAPTURE(back);
        CHECK(back >= CLK_6502_NTSC * (1 - RealtimeRateTolerance()));
        CHECK(back <= CLK_6502_NTSC * (1 + RealtimeRateTolerance()));
    }

    TEST_CASE("speed: a speed change mid-run reaches the new rate at once (Review Focus 3)")
    {
        auto& e = StartedEmulator();
        RestoreSpeed restore{e};
        NoMotorFullSpeed(e);
        e.SetMachineSpeed(10);
        REQUIRE(e.RunRealtime(1000ms));
        e.SetMachineSpeed(20);
        const double rate = MeasureRate(e, 0ms, 3000ms);   // no settle: the change must take effect within a tick
        CAPTURE(rate);
        CHECK(rate >= 2.0 * CLK_6502_NTSC * (1 - RealtimeRateTolerance()));
        CHECK(rate <= 2.0 * CLK_6502_NTSC * (1 + RealtimeRateTolerance()));
    }

    TEST_CASE("speed: disk_motor is a full-speed reason during DOS boot and gone at the prompt (Review Focus 2)")
    {
        auto& e = StartedEmulator();
        RestoreSpeed restore{e};
        const auto work =
            std::filesystem::temp_directory_path() / ("awh-speed-motor-" + std::to_string(GetCurrentProcessId()));
        WorkingCopy copy(DosMasterImage(), work);
        e.InsertDisk(0, copy.Path());
        EjectAndPowerCycle ejector{e};

        e.SetMachineSpeed(10);
        e.PowerCycle();
        bool sawMotor = false;
        const auto deadline = std::chrono::steady_clock::now() + 40s;
        while (std::chrono::steady_clock::now() < deadline && !(sawMotor && AtPrompt(e))) {
            REQUIRE(e.RunRealtime(20ms));
            const auto reasons = e.FullSpeedReasons();
            CHECK(InCanonicalOrder(reasons));
            if (Contains(reasons, "disk_motor")) {
                sawMotor = true;
                CHECK(e.FullSpeedActive());
            }
        }
        CHECK(sawMotor);
        REQUIRE(AtPrompt(e));
        REQUIRE(e.RunRealtime(3000ms));   // the motor spins down after about a second
        CHECK(e.FullSpeedReasons().empty());
        CHECK_FALSE(e.FullSpeedActive());

        // enhance_disk_speed off: the motor is no longer a reason.
        e.SetEnhanceDiskSpeed(false);
        e.PowerCycle();
        // Positive control: run until DOS has booted from the disk (so the motor really ran) and check every slice.
        bool sawMotorWhileOff = false;
        const auto offDeadline = std::chrono::steady_clock::now() + 40s;
        while (std::chrono::steady_clock::now() < offDeadline && !AtPrompt(e)) {
            REQUIRE(e.RunRealtime(20ms));
            if (Contains(e.FullSpeedReasons(), "disk_motor")) sawMotorWhileOff = true;
        }
        CHECK(AtPrompt(e));   // the boot read the disk, so the motor was on
        CHECK_FALSE(sawMotorWhileOff);
    }

    TEST_CASE("speed: all three reasons are reported in order while the motor runs at max with the override")
    {
        auto& e = StartedEmulator();
        RestoreSpeed restore{e};
        const auto work =
            std::filesystem::temp_directory_path() / ("awh-speed-order-" + std::to_string(GetCurrentProcessId()));
        WorkingCopy copy(DosMasterImage(), work);
        e.InsertDisk(0, copy.Path());
        EjectAndPowerCycle ejector{e};

        e.SetMachineSpeed(40);
        e.SetFullSpeedOverride(true);
        e.PowerCycle();
        bool sawAll = false;
        for (int i = 0; i < 400 && !sawAll; ++i) {
            REQUIRE(e.RunRealtime(5ms));
            const auto reasons = e.FullSpeedReasons();
            CHECK(InCanonicalOrder(reasons));
            if (reasons.size() == 3) {
                CHECK(reasons == std::vector<std::string>{"machine_speed_max", "override", "disk_motor"});
                sawAll = true;
            }
        }
        CHECK(sawAll);
    }
}
