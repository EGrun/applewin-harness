// M3 Part C (D11): speed controls. Spec: docs/specs/m3.md Part C, docs/api_spec.md "Speed".
// The in-process cases ("speed: ...") are fast and deterministic. The rate and full-speed cases are in suite
// "realtime" (headless, wall-clock timing).
#include "doctest/doctest.h"
#include "host/emulator.h"
#include "core/sha256.h"
#include "core/working_copy.h"
#include "support/emulator_fixture.h"
#include "support/paths.h"

#pragma warning(push, 3)
#include "Common.h"
#include "Core.h"
#pragma warning(pop)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

using namespace harness;
using namespace harness::testsupport;
using namespace std::chrono_literals;

namespace {

// Back to the M1 state however a test ends: authentic speed, no override, enhanced disk speed on. Never throws; the
// speed is restored through AppleWin directly, so it still works while the Emulator speed API is a stub.
struct RestoreSpeed {
    Emulator& e;
    ~RestoreSpeed()
    {
        try { e.SetFullSpeedOverride(false); } catch (...) {}
        try { e.SetEnhanceDiskSpeed(true); } catch (...) {}
        g_dwSpeed = SPEED_NORMAL;
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

std::string BootAndPrintHash(Emulator& e)
{
    e.PowerCycle();
    REQUIRE(e.RunFrames(720));
    REQUIRE(e.TypeText("PRINT 2+2\r"));
    REQUIRE(e.RunFrames(60));
    return FrameHash(e.Frame());
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
    CHECK(e.FullSpeedReasons().empty());
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
    CHECK(e.FullSpeedReasons().empty());
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
    const std::string at5 = BootAndPrintHash(e);
    const uint64_t cycles5 = e.EmulatedCycles();
    e.SetMachineSpeed(40);
    const std::string at40 = BootAndPrintHash(e);
    const uint64_t cycles40 = e.EmulatedCycles();
    e.SetMachineSpeed(10);
    e.SetFullSpeedOverride(true);
    const std::string withOverride = BootAndPrintHash(e);
    CHECK(at5 == at40);
    CHECK(at5 == withOverride);
    CHECK(cycles5 == cycles40);
}

TEST_SUITE("realtime")
{
    TEST_CASE("speed: the rate at machine_speed 5, 10 and 20 is within 2% of EffectiveClockHz (pass condition 10)")
    {
        auto& e = StartedEmulator();
        RestoreSpeed restore{e};
        for (const uint32_t speed : {5u, 10u, 20u}) {
            CAPTURE(speed);
            e.SetMachineSpeed(speed);
            const double hz = e.EffectiveClockHz();
            const double rate = MeasureRate(e, 1000ms, 3000ms);
            CAPTURE(rate);
            CAPTURE(hz);
            CHECK(rate >= hz * 0.98);
            CHECK(rate <= hz * 1.02);
        }
    }

    TEST_CASE("speed: max and the override each run over 2x authentic (pass condition 11)")
    {
        auto& e = StartedEmulator();
        RestoreSpeed restore{e};
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
        CHECK(back >= CLK_6502_NTSC * 0.98);
        CHECK(back <= CLK_6502_NTSC * 1.02);
    }

    TEST_CASE("speed: a speed change mid-run reaches the new rate at once (Review Focus 3)")
    {
        auto& e = StartedEmulator();
        RestoreSpeed restore{e};
        e.SetMachineSpeed(10);
        REQUIRE(e.RunRealtime(1000ms));
        e.SetMachineSpeed(20);
        const double rate = MeasureRate(e, 0ms, 3000ms);   // no settle: the change must take effect within a tick
        CAPTURE(rate);
        CHECK(rate >= 2.0 * CLK_6502_NTSC * 0.98);
        CHECK(rate <= 2.0 * CLK_6502_NTSC * 1.02);
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
        bool sawMotorWhileOff = false;
        for (int i = 0; i < 100; ++i) {
            REQUIRE(e.RunRealtime(20ms));
            if (Contains(e.FullSpeedReasons(), "disk_motor")) sawMotorWhileOff = true;
        }
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
