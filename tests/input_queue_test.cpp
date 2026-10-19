// SPDX-License-Identifier: MIT
// Emulator input queue (spec: docs/specs/m3.md, Part A "Input queue"; D10; pass condition 4 in-process).
#include "doctest/doctest.h"
#include "host/emulator.h"
#include "core/sha256.h"
#include "core/working_copy.h"
#include "support/emulator_fixture.h"
#include "support/golden.h"
#include "support/paths.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#pragma warning(push, 3)
#include "Keyboard.h"
#pragma warning(pop)

using namespace harness;
using namespace harness::testsupport;

namespace {

// CLK_6502_NTSC (AppleWin Common.h): the base clock that converts emulated milliseconds to cycles.
constexpr double kBaseClockHz = (157500000.0 / 11.0) * 65.0 / 912.0;
constexpr uint64_t kCyclesPerFrame = 17030;

constexpr uint64_t MsToCycles(double ms)
{
    return uint64_t(ms * kBaseClockHz / 1000.0);   // floor: the lower bound any rounding satisfies
}

// Strips spaces and the enhanced //e 40-column cursor (0x7F after & 0x7F), as in dos_test.cpp.
std::string TrimRow(const std::string& s)
{
    auto blank = [](char c) { return c == ' ' || c == '\x7f'; };
    size_t b = 0, e = s.size();
    while (b < e && blank(s[b])) ++b;
    while (e > b && blank(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> Rows(const Emulator& e)
{
    std::vector<std::string> rows;
    for (int r = 0; r < 24; ++r)
        rows.push_back(TrimRow(e.ReadTextRow(r)));
    return rows;
}

std::string Screen(const std::vector<std::string>& rows)
{
    std::string s;
    for (const auto& row : rows) s += "\n|" + row;
    return s;
}

std::string LastNonEmptyRow(const std::vector<std::string>& rows)
{
    for (auto it = rows.rbegin(); it != rows.rend(); ++it)
        if (!it->empty()) return *it;
    return {};
}

// The row after the last row equal to line, or "<missing>".
std::string RowAfterLast(const std::vector<std::string>& rows, const std::string& line)
{
    for (size_t r = rows.size(); r-- > 0;)
        if (rows[r] == line) return r + 1 < rows.size() ? rows[r + 1] : std::string("<missing>");
    return "<missing>";
}

std::optional<InputApplied> FindApplied(const std::vector<InputApplied>& applied, uint64_t id)
{
    for (const auto& a : applied)
        if (a.inputId == id) return a;
    return std::nullopt;
}

JoystickChange X(uint8_t x)
{
    JoystickChange c;
    c.x = x;
    return c;
}

JoystickChange Button0(bool down)
{
    JoystickChange c;
    c.button0 = down;
    return c;
}

JoystickChange Centred()
{
    JoystickChange c;
    c.x = uint8_t(127);
    c.y = uint8_t(127);
    c.button0 = false;
    c.button1 = false;
    return c;
}

// Power-cycles on the way out, so the M1 boot tests (no disk) that follow stay valid. Never throws.
struct PowerCycleOnExit {
    Emulator& e;
    ~PowerCycleOnExit()
    {
        try { e.PowerCycle(); } catch (...) {}
    }
};

// DOS 3.3 booted from a working copy (as in dos_test.cpp): insert, power-cycle, 720 frames. The destructor ejects and
// power-cycles (before the copy is deleted) so the M1 boot tests stay valid. Never throws from the destructor.
class DosBasic {
public:
    explicit DosBasic(Emulator& e)
        : e_(e),
          copy_(DosMasterImage(),
                std::filesystem::temp_directory_path() / ("awh-input-test-" + std::to_string(GetCurrentProcessId())))
    {
        e_.InsertDisk(0, copy_.Path());
        e_.PowerCycle();
        booted_ = e_.RunFrames(720);
    }
    ~DosBasic()
    {
        try { e_.EjectDisk(0); } catch (...) {}
        try { e_.PowerCycle(); } catch (...) {}
    }
    DosBasic(const DosBasic&) = delete;
    DosBasic& operator=(const DosBasic&) = delete;

    bool Booted() const { return booted_; }

private:
    Emulator& e_;
    WorkingCopy copy_;
    bool booted_ = false;
};

}  // namespace

TEST_CASE("input: QueueText then 60 frames gives the M2 golden")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    (void)e.TakeAppliedInputs();

    const uint64_t id = e.QueueText("PRINT 2+2\r", 0);
    CHECK(id != 0);
    REQUIRE(e.RunFrames(60));

    const auto rows = Rows(e);
    INFO("text page 1 (trimmed):" << Screen(rows));
    bool found = false;
    for (size_t r = 0; r + 3 < rows.size() && !found; ++r)
        found = rows[r] == "]PRINT 2+2" && rows[r + 1] == "4" && rows[r + 2].empty() && rows[r + 3] == "]";
    CHECK(found);

    const std::string golden = ReadGoldenTrimmed("m2_dos_print.sha256");
    REQUIRE_MESSAGE(!golden.empty(), "golden m2_dos_print.sha256 missing");
    CHECK(FrameHash(e.Frame()) == golden);

    const auto applied = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE_MESSAGE(applied.has_value(), "no InputApplied for the text");
    CHECK(applied->firstCycle <= applied->lastCycle);
    CHECK(applied->lastCycle <= e.EmulatedCycles());
}

TEST_CASE("input: keys are first in, first out and wait for the strobe")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    (void)e.TakeAppliedInputs();

    KeybQueueKeypress(WPARAM('X'), ASCII);   // a key the program has not read yet: the strobe is set
    REQUIRE((KeybReadData() & 0x80) != 0);
    const uint64_t ab = e.QueueText("AB", 0);
    CHECK(e.ScheduledCycle(ab) == std::nullopt);   // a key waits for the strobe: not known yet
    const uint64_t c = e.QueueText("C", 0);
    CHECK(c > ab);
    CHECK(e.ScheduledCycle(c) == std::nullopt);    // queued behind "AB"
    REQUIRE(e.RunFrames(30));

    const auto rows = Rows(e);
    INFO("text page 1 (trimmed):" << Screen(rows));
    CHECK(LastNonEmptyRow(rows) == "]XABC");

    const auto applied = e.TakeAppliedInputs();
    const auto a1 = FindApplied(applied, ab), a2 = FindApplied(applied, c);
    REQUIRE(a1.has_value());
    REQUIRE(a2.has_value());
    CHECK(a1->firstCycle < a1->lastCycle);
    CHECK(a1->lastCycle < a2->firstCycle);
    CHECK(e.ScheduledCycle(ab) == std::optional<uint64_t>(a1->firstCycle));   // once applied: its first cycle
}

TEST_CASE("input: inter_key_delay_ms spaces the keys at the base clock")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    (void)e.TakeAppliedInputs();

    const uint64_t spaced = e.QueueText("AB", 30);
    REQUIRE(e.RunFrames(30));
    const auto s = FindApplied(e.TakeAppliedInputs(), spaced);
    REQUIRE(s.has_value());
    CHECK(s->lastCycle - s->firstCycle >= MsToCycles(30));

    // With no delay the next key follows as soon as BASIC has read the previous one: well under 30 ms.
    const uint64_t fast = e.QueueText("CD", 0);
    REQUIRE(e.RunFrames(30));
    const auto f = FindApplied(e.TakeAppliedInputs(), fast);
    REQUIRE(f.has_value());
    CHECK(f->lastCycle - f->firstCycle < MsToCycles(30));

    const auto rows = Rows(e);
    INFO("text page 1 (trimmed):" << Screen(rows));
    CHECK(LastNonEmptyRow(rows) == "]ABCD");
}

TEST_CASE("input: press_key holds the any-key-down flag for hold_ms")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));
    (void)e.TakeAppliedInputs();

    // $C010 read: clears the strobe and returns the any-key-down flag in bit 7 (AppleWin's KeybReadFlag).
    REQUIRE((KeybReadFlag() & 0x80) == 0);
    const uint64_t id = e.QueueKey(KeySpec(uint8_t('A')), 100);
    CHECK(e.ScheduledCycle(id) == std::optional<uint64_t>(e.EmulatedCycles()));   // queue empty, strobe clear
    REQUIRE(e.RunFrames(1));   // ~17 ms
    CHECK((KeybReadFlag() & 0x80) != 0);
    REQUIRE(e.RunFrames(3));   // ~67 ms
    CHECK((KeybReadFlag() & 0x80) != 0);
    CHECK(!FindApplied(e.TakeAppliedInputs(), id).has_value());   // input_applied comes with the release
    REQUIRE(e.RunFrames(4));   // ~133 ms
    CHECK((KeybReadFlag() & 0x80) == 0);

    const auto a = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(a.has_value());
    CHECK(a->lastCycle - a->firstCycle >= MsToCycles(100));
    CHECK(a->lastCycle - a->firstCycle < MsToCycles(100) + kCyclesPerFrame);
}

TEST_CASE("input: named keys reach the keyboard latch")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));   // nothing reads the keyboard while the Disk II boot ROM waits for a disk

    struct Case {
        KeySpec key;
        BYTE expected;   // KeybReadData(): the key code with the strobe (bit 7)
    } cases[] = {
        {ArrowKey::Left, 0x88}, {ArrowKey::Right, 0x95}, {ArrowKey::Up, 0x8B}, {ArrowKey::Down, 0x8A},
        {uint8_t(0x0D), 0x8D},  {uint8_t(0x01), 0x81},   {uint8_t(0x7F), 0xFF}, {uint8_t('Z'), 0xDA},
    };
    for (const auto& c : cases) {
        KeybClearStrobe();
        e.QueueKey(c.key, 0);
        REQUIRE(e.RunFrames(1));
        CHECK(int(KeybReadData()) == int(c.expected));
    }
    KeybClearStrobe();
}

TEST_CASE("input: press_key overwrites a key the program has not read (D10)")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));   // nothing reads the keyboard while the Disk II boot ROM waits for a disk
    (void)e.TakeAppliedInputs();

    KeybQueueKeypress(WPARAM('X'), ASCII);   // unread: the strobe is set
    REQUIRE((KeybReadData() & 0x80) != 0);
    const uint64_t id = e.QueueKey(KeySpec(uint8_t('Y')), 0);
    CHECK(e.ScheduledCycle(id) == std::optional<uint64_t>(e.EmulatedCycles()));   // does not wait for the strobe
    REQUIRE(e.RunFrames(1));
    CHECK(int(KeybReadData()) == int(0x80 | 'Y'));
    CHECK(FindApplied(e.TakeAppliedInputs(), id).has_value());
    KeybClearStrobe();
}

TEST_CASE("input: an unread type_text times out and the queue behind it drains (D10)")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));   // nothing reads the keyboard
    (void)e.TakeAppliedInputs();
    KeybClearStrobe();

    const uint64_t text = e.QueueText("AB", 0);   // "A" is applied; "B" then waits for the strobe for ever
    const uint64_t key = e.QueueKey(KeySpec(uint8_t('Z')), 0);
    const uint64_t joy = e.QueueJoystick(X(10), 0);
    REQUIRE(e.RunFrames(598));
    {
        const auto applied = e.TakeAppliedInputs();
        CHECK(applied.empty());   // still waiting: nothing behind the text has moved
        CHECK(int(KeybReadData()) == int(0x80 | 'A'));
    }
    REQUIRE(e.RunFrames(4));   // past 600 frames since "B" was due
    const auto applied = e.TakeAppliedInputs();
    const auto t = FindApplied(applied, text), k = FindApplied(applied, key), j = FindApplied(applied, joy);
    REQUIRE(t.has_value());
    CHECK(t->discarded);
    REQUIRE(k.has_value());
    CHECK(!k->discarded);
    REQUIRE(j.has_value());
    CHECK(!j->discarded);
    CHECK(int(KeybReadData()) == int(0x80 | 'Z'));   // "B" never typed; the key behind it overwrote "A"
    CHECK(e.PendingInputEvents() == 0);
    KeybClearStrobe();
    e.QueueJoystick(Centred(), 0);
    CHECK(e.RunFrames(1));
}

TEST_CASE("input: overlapping holds of the same key keep the any-key-down flag until the last release")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));
    (void)e.TakeAppliedInputs();

    e.QueueKey(KeySpec(uint8_t('A')), 100);
    REQUIRE(e.RunFrames(3));   // ~50 ms
    e.QueueKey(KeySpec(uint8_t('A')), 100);   // pressed again while the first is held: released at ~150 ms
    REQUIRE(e.RunFrames(4));   // ~117 ms: the first hold has ended
    CHECK((KeybReadFlag() & 0x80) != 0);
    REQUIRE(e.RunFrames(4));   // ~183 ms
    CHECK((KeybReadFlag() & 0x80) == 0);
}

TEST_CASE("input: TypeText throws when the keyboard is not read and leaves nothing queued")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));   // nothing reads the keyboard
    (void)e.TakeAppliedInputs();
    KeybClearStrobe();

    CHECK_THROWS_AS(e.TypeText("AB"), std::runtime_error);
    CHECK(e.PendingInputEvents() == 0);
    CHECK(e.TakeAppliedInputs().empty());   // TypeText consumes its own report
    KeybClearStrobe();
}

TEST_CASE("input: RESET returns to the BASIC prompt without waiting for the strobe")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    (void)e.TakeAppliedInputs();

    e.QueueText("PRINT", 0);
    REQUIRE(e.RunFrames(30));
    {
        const auto rows = Rows(e);
        INFO("before RESET:" << Screen(rows));
        REQUIRE(LastNonEmptyRow(rows) == "]PRINT");
    }

    KeybQueueKeypress(WPARAM('X'), ASCII);   // strobe set: an ordinary key would have to wait
    const uint64_t id = e.QueueKey(ResetKey{}, 0);
    CHECK(e.ScheduledCycle(id) == std::optional<uint64_t>(e.EmulatedCycles()));
    REQUIRE(e.RunFrames(60));

    const auto rows = Rows(e);
    INFO("after RESET:" << Screen(rows));
    CHECK(LastNonEmptyRow(rows) == "]");
    CHECK(std::find(rows.begin(), rows.end(), "]PRINT") != rows.end());
    CHECK(FindApplied(e.TakeAppliedInputs(), id).has_value());
}

TEST_CASE("input: joystick 0 drives PDL(0) and button 0")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    (void)e.TakeAppliedInputs();

    // Types PRINT <expr> and checks the line BASIC prints.
    auto check = [&](const std::string& expr, const std::string& expected) {
        e.QueueText("PRINT " + expr + "\r", 0);
        REQUIRE(e.RunFrames(60));
        const auto rows = Rows(e);
        INFO("text page 1 (trimmed):" << Screen(rows));
        CHECK(RowAfterLast(rows, "]PRINT " + expr) == expected);
    };

    e.QueueJoystick(X(0), 0);
    check("PDL(0)", "0");
    e.QueueJoystick(X(255), 0);
    check("PDL(0)", "255");
    e.QueueJoystick(Button0(true), 0);
    check("PEEK(-16287)>127", "1");
    e.QueueJoystick(Button0(false), 0);
    check("PEEK(-16287)>127", "0");

    e.QueueJoystick(Centred(), 0);
    CHECK(e.RunFrames(1));
}

TEST_CASE("input: applied_at_cycle and InputApplied for joystick changes")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));
    (void)e.TakeAppliedInputs();

    // Nothing queued: takes effect at the start of the next step, i.e. at the current cycle (D10).
    const uint64_t now = e.EmulatedCycles();
    const uint64_t id = e.QueueJoystick(X(10), 0);
    CHECK(e.ScheduledCycle(id) == std::optional<uint64_t>(now));
    REQUIRE(e.RunFrames(1));
    const auto applied = e.TakeAppliedInputs();
    REQUIRE(applied.size() == 1);
    CHECK(applied[0].inputId == id);
    CHECK(applied[0].firstCycle == now);
    CHECK(applied[0].lastCycle == now);
    CHECK(e.ScheduledCycle(id) == std::optional<uint64_t>(now));

    // duration_ms: InputApplied arrives with the restore, duration_ms later.
    const uint64_t timed = e.QueueJoystick(X(20), 50);
    CHECK(timed > id);
    REQUIRE(e.RunFrames(1));   // ~17 ms
    CHECK(e.TakeAppliedInputs().empty());
    REQUIRE(e.RunFrames(3));   // ~67 ms
    const auto t = FindApplied(e.TakeAppliedInputs(), timed);
    REQUIRE(t.has_value());
    CHECK(t->lastCycle - t->firstCycle >= MsToCycles(50));

    e.QueueJoystick(Centred(), 0);
    CHECK(e.RunFrames(1));
}

TEST_CASE("input: invalid input throws std::invalid_argument and queues nothing")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));
    KeybClearStrobe();

    CHECK_THROWS_AS(e.QueueText("A\x01", 0), std::invalid_argument);
    CHECK_THROWS_AS(e.QueueText("\x7f", 0), std::invalid_argument);
    CHECK_THROWS_AS(e.QueueText("caf\xc3\xa9", 0), std::invalid_argument);
    CHECK_THROWS_AS(e.QueueKey(KeySpec(uint8_t(128)), 0), std::invalid_argument);
    REQUIRE(e.RunFrames(1));
    CHECK((KeybReadData() & 0x80) == 0);   // the valid "A" before the bad byte was not queued
    CHECK(e.ScheduledCycle(0) == std::nullopt);   // 0 is never an input id
}
