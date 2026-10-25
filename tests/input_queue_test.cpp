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
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
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

TEST_CASE("press_keys: each key waits for the previous read and its delay, and reports applied and read cycles")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    (void)e.TakeAppliedInputs();

    // "B" 30 ms after "A"; "C" as soon as BASIC has read "B" (delay 0 never overwrites an unread key).
    const uint64_t id = e.QueueKeys({{KeySpec(uint8_t('A')), 0, 0}, {KeySpec(uint8_t('B')), 30, 0},
                                     {KeySpec(uint8_t('C')), 0, 0}});
    CHECK(e.ScheduledCycle(id) == std::optional<uint64_t>(e.EmulatedCycles()));   // strobe clear, no first delay
    REQUIRE(e.RunFrames(30));

    const auto rows = Rows(e);
    INFO("text page 1 (trimmed):" << Screen(rows));
    CHECK(LastNonEmptyRow(rows) == "]ABC");

    const auto a = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(a.has_value());
    CHECK(!a->discarded);
    REQUIRE(a->keys.size() == 3);
    for (const KeyTiming& k : a->keys)
        CHECK(k.appliedCycle <= k.readCycle);   // the read's own cycle: BASIC polls in a tight loop
    CHECK(a->firstCycle == a->keys[0].appliedCycle);
    CHECK(a->keys[1].appliedCycle >= a->keys[0].appliedCycle + MsToCycles(30));
    CHECK(a->keys[1].appliedCycle >= a->keys[0].readCycle + MsToCycles(1));
    CHECK(a->keys[1].appliedCycle < a->keys[0].appliedCycle + MsToCycles(30) + 8);   // on its due cycle
    // "C" waits for BASIC's read of "B", and then 1 ms more: BASIC checks $C000 and fetches the key a few
    // instructions later, so a key put in right after the check would replace the one it is about to fetch.
    CHECK(a->keys[2].appliedCycle >= a->keys[1].readCycle + MsToCycles(1));
    CHECK(a->keys[2].appliedCycle < a->keys[1].readCycle + MsToCycles(1) + 8);
    CHECK(a->lastCycle == a->keys[2].readCycle);
}

TEST_CASE("press_keys: a hold may run past the next key, and the sequence completes at the last release")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    (void)e.TakeAppliedInputs();

    const uint64_t id = e.QueueKeys({{KeySpec(uint8_t('A')), 0, 200}, {KeySpec(uint8_t('B')), 0, 0}});
    REQUIRE(e.RunFrames(6));   // ~100 ms: "B" pressed and read, "A" still held
    CHECK(!FindApplied(e.TakeAppliedInputs(), id).has_value());
    REQUIRE(e.RunFrames(12));   // ~300 ms
    const auto a = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(a.has_value());
    REQUIRE(a->keys.size() == 2);
    CHECK(a->keys[1].appliedCycle < a->keys[0].appliedCycle + MsToCycles(200));
    CHECK(a->lastCycle >= a->keys[0].appliedCycle + MsToCycles(200));
    CHECK(a->lastCycle < a->keys[0].appliedCycle + MsToCycles(200) + MsToCycles(2));

    const auto rows = Rows(e);
    INFO("text page 1 (trimmed):" << Screen(rows));
    CHECK(LastNonEmptyRow(rows) == "]AB");
}

TEST_CASE("press_keys: a last key left unread for 600 frames discards the sequence and the queue drains")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));   // nothing reads the keyboard
    (void)e.TakeAppliedInputs();
    KeybClearStrobe();

    const uint64_t id = e.QueueKeys({{KeySpec(ArrowKey::Left), 0, 0}, {KeySpec(ArrowKey::Right), 0, 0}});
    const uint64_t joy = e.QueueJoystick(X(10), 0);
    REQUIRE(e.RunFrames(598));
    CHECK(e.TakeAppliedInputs().empty());   // waiting for "LEFT" to be read; the joystick change waits behind it
    REQUIRE(e.RunFrames(4));
    const auto applied = e.TakeAppliedInputs();
    const auto a = FindApplied(applied, id), j = FindApplied(applied, joy);
    REQUIRE(a.has_value());
    CHECK(a->discarded);
    REQUIRE(a->keys.size() == 1);   // the keys pressed: "LEFT", never read; "RIGHT" was never pressed
    CHECK(a->keys[0].appliedCycle > 0);
    CHECK(a->keys[0].readCycle == 0);
    REQUIRE(j.has_value());
    CHECK(e.PendingInputEvents() == 0);
    KeybClearStrobe();
    e.QueueJoystick(Centred(), 0);
    CHECK(e.RunFrames(1));
}

TEST_CASE("press_keys: a first-key delay counts from the queueing, and the applied cycle is then not known")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));
    (void)e.TakeAppliedInputs();
    KeybClearStrobe();

    const uint64_t queuedAt = e.EmulatedCycles();
    const uint64_t id = e.QueueKeys({{KeySpec(uint8_t('A')), 50, 0}});
    CHECK(e.ScheduledCycle(id) == std::nullopt);
    REQUIRE(e.RunFrames(2));   // ~33 ms
    CHECK((KeybReadData() & 0x80) == 0);
    REQUIRE(e.RunFrames(2));   // ~67 ms
    CHECK(int(KeybReadData()) == int(0x80 | 'A'));
    const auto first = e.ScheduledCycle(id);
    REQUIRE(first.has_value());
    CHECK(*first >= queuedAt + MsToCycles(50));
    CHECK(*first < queuedAt + MsToCycles(52));
    KeybClearStrobe();
}

TEST_CASE("press_keys: the first key does not wait for a key an earlier input left unread")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));   // nothing reads the keyboard
    (void)e.TakeAppliedInputs();

    KeybQueueKeypress(WPARAM('X'), ASCII);   // a stale key: the strobe is set
    const uint64_t id = e.QueueKeys({{KeySpec(uint8_t('Y')), 0, 0}});
    CHECK(e.ScheduledCycle(id) == std::optional<uint64_t>(e.EmulatedCycles()));
    REQUIRE(e.RunFrames(1));
    CHECK(int(KeybReadData()) == int(0x80 | 'Y'));   // replaced, as press_key does
    KeybClearStrobe();
}

TEST_CASE("press_keys: a keyboard read counts as read even when the program never clears the strobe")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    e.QueueText("10 GOTO 10\rRUN\r", 0);
    REQUIRE(e.RunFrames(60));
    (void)e.TakeAppliedInputs();

    // Applesoft's Ctrl-C check (ISCNTC) reads $C000 between statements and never clears the strobe for other keys.
    const uint64_t id = e.QueueKeys({{KeySpec(ArrowKey::Left), 0, 0}, {KeySpec(ArrowKey::Left), 0, 0},
                                     {KeySpec(uint8_t('Q')), 0, 0}});
    REQUIRE(e.RunFrames(10));
    const auto a = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(a.has_value());
    CHECK(!a->discarded);
    REQUIRE(a->keys.size() == 3);
    for (size_t i = 0; i < a->keys.size(); ++i) {
        CHECK(a->keys[i].appliedCycle < a->keys[i].readCycle);
        CHECK(a->keys[i].readCycle - a->keys[i].appliedCycle < MsToCycles(2));   // read within a step or two
        if (i > 0)
            CHECK(a->keys[i].appliedCycle >= a->keys[i - 1].readCycle);
    }
    CHECK((KeybReadData() & 0x80) != 0);   // the strobe is still set: only the reads counted
    e.QueueKey(ResetKey{}, 0);
    CHECK(e.RunFrames(30));
}

TEST_CASE("press_keys: invalid sequences throw std::invalid_argument and queue nothing")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));

    CHECK_THROWS_AS(e.QueueKeys({}), std::invalid_argument);
    CHECK_THROWS_AS(e.QueueKeys({{KeySpec(uint8_t('A')), 0, 0}, {KeySpec(ResetKey{}), 0, 0}}), std::invalid_argument);
    CHECK_THROWS_AS(e.QueueKeys({{KeySpec(uint8_t(128)), 0, 0}}), std::invalid_argument);
    CHECK(e.PendingInputEvents() == 0);
}

namespace {
// A press_keys joystick entry.
SequenceKey JoyEntry(JoystickChange change, uint32_t delayMs, uint32_t durationMs)
{
    SequenceKey k;
    k.joystick = change;
    k.delayMs = delayMs;
    k.durationMs = durationMs;
    return k;
}
}  // namespace

TEST_CASE("press_keys: a joystick entry applies after its delay and restores after its duration")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));   // nothing reads the keyboard
    (void)e.TakeAppliedInputs();

    const uint64_t queuedAt = e.EmulatedCycles();
    const uint64_t id = e.QueueKeys({JoyEntry(X(10), 20, 50)});
    CHECK(e.PendingInputEvents() == 2);
    REQUIRE(e.RunFrames(1));   // ~17 ms
    CHECK(e.ScheduledCycle(id) == std::nullopt);
    REQUIRE(e.RunFrames(2));
    const auto first = e.ScheduledCycle(id);
    REQUIRE(first.has_value());
    CHECK(*first >= queuedAt + MsToCycles(20));
    CHECK(!FindApplied(e.TakeAppliedInputs(), id).has_value());   // the restore is pending
    CHECK(e.PendingInputEvents() == 1);
    REQUIRE(e.RunFrames(4));
    const auto a = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(a.has_value());
    CHECK(!a->discarded);
    REQUIRE(a->keys.size() == 1);
    CHECK(a->keys[0].joystick);
    CHECK(a->keys[0].readCycle == 0);
    CHECK(a->lastCycle - a->firstCycle >= MsToCycles(50));
    CHECK(e.PendingInputEvents() == 0);
}

TEST_CASE("press_keys: a key after a joystick entry waits for the last key's read")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    (void)e.TakeAppliedInputs();

    SequenceKey a, b;
    a.key = KeySpec(uint8_t('A'));
    a.holdMs = 0;
    b.key = KeySpec(uint8_t('B'));
    b.holdMs = 0;
    const uint64_t id = e.QueueKeys({a, JoyEntry(X(0), 0, 0), b});
    REQUIRE(e.RunFrames(30));

    const auto rows = Rows(e);
    INFO("text page 1 (trimmed):" << Screen(rows));
    CHECK(LastNonEmptyRow(rows) == "]AB");
    const auto r = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(r.has_value());
    CHECK(!r->discarded);
    REQUIRE(r->keys.size() == 3);
    CHECK(!r->keys[0].joystick);
    CHECK(r->keys[1].joystick);
    CHECK(!r->keys[2].joystick);
    CHECK(r->keys[2].appliedCycle >= r->keys[0].readCycle);
    CHECK(r->keys[1].appliedCycle <= r->keys[2].appliedCycle);
    CHECK(r->keys[0].readCycle != 0);
    CHECK(r->keys[2].readCycle != 0);
    e.QueueJoystick(Centred(), 0);
    CHECK(e.RunFrames(1));
}

TEST_CASE("press_keys: wait_for_read false overwrites an unread key")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));   // nothing reads the keyboard
    (void)e.TakeAppliedInputs();
    KeybClearStrobe();

    SequenceKey a, b;
    a.key = KeySpec(uint8_t('A'));
    a.holdMs = 0;
    b.key = KeySpec(uint8_t('B'));
    b.holdMs = 0;
    b.waitForRead = false;
    const uint64_t id = e.QueueKeys({a, b});
    REQUIRE(e.RunFrames(1));
    CHECK(int(KeybReadData()) == int(0x80 | 'B'));
    REQUIRE(e.RunFrames(601));
    const auto r = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(r.has_value());
    CHECK(r->discarded);   // "B" was never read
    REQUIRE(r->keys.size() == 2);
    CHECK(r->keys[0].readCycle == 0);
    CHECK(r->keys[1].readCycle == 0);
    CHECK(e.PendingInputEvents() == 0);
    KeybClearStrobe();
}

TEST_CASE("press_keys: a key superseded by a wait_for_read false key stays unread without failing the sequence")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    (void)e.TakeAppliedInputs();

    // BASIC reads the keyboard only while a step runs, and the entries are applied at one step boundary: with delay 0
    // and no wait, "B" replaces "A" before BASIC can have read it. "B" is the key that is read.
    SequenceKey a, b;
    a.key = KeySpec(uint8_t('A'));
    a.holdMs = 0;
    b.key = KeySpec(uint8_t('B'));
    b.holdMs = 0;
    b.waitForRead = false;
    const uint64_t id = e.QueueKeys({a, b});
    REQUIRE(e.RunFrames(30));   // far below the 600-frame timeout

    const auto r = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(r.has_value());
    CHECK(!r->discarded);
    REQUIRE(r->keys.size() == 2);
    CHECK(r->keys[0].appliedCycle == r->keys[1].appliedCycle);   // same step boundary: "A" was never readable
    CHECK(r->keys[0].readCycle == 0);
    CHECK(r->keys[1].readCycle != 0);
    CHECK(LastNonEmptyRow(Rows(e)) == "]B");
    CHECK(e.PendingInputEvents() == 0);
}

TEST_CASE("press_keys: only a key a waiting entry depends on, or the last key, can time the sequence out")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));   // nothing reads the keyboard
    (void)e.TakeAppliedInputs();
    KeybClearStrobe();

    // "A" sits unread for ~600.5 frames (10 s) before "M" replaces it, and "M" for as long before "B" does: neither
    // may time the sequence out. "C" waits for "B", which nothing reads: only that one does, 600 frames after it.
    SequenceKey a, m, b, c;
    a.key = KeySpec(uint8_t('A'));
    a.holdMs = 0;
    m.key = KeySpec(uint8_t('M'));
    m.holdMs = 0;
    m.delayMs = 10000;
    m.waitForRead = false;
    b.key = KeySpec(uint8_t('B'));
    b.holdMs = 0;
    b.delayMs = 10000;
    b.waitForRead = false;
    c.key = KeySpec(uint8_t('C'));
    c.holdMs = 0;
    const uint64_t id = e.QueueKeys({a, m, b, c});
    REQUIRE(e.RunFrames(1000));   // "A" has been unread for 1000 frames, "M" for ~400
    CHECK(e.TakeAppliedInputs().empty());
    REQUIRE(e.RunFrames(250));    // 1250: "B" was applied at ~1201
    CHECK(e.TakeAppliedInputs().empty());
    REQUIRE(e.RunFrames(560));    // 1810: "B" has been unread for 600 frames
    const auto r = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(r.has_value());
    CHECK(r->discarded);
    REQUIRE(r->keys.size() == 3);   // "C" was never pressed
    CHECK(r->keys[0].readCycle == 0);
    CHECK(r->keys[1].readCycle == 0);
    CHECK(r->keys[2].readCycle == 0);
    CHECK(e.PendingInputEvents() == 0);
    KeybClearStrobe();
}

TEST_CASE("press_keys: overlapping joystick durations restore in due order, each to the state before its own entry")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    (void)e.TakeAppliedInputs();

    // x=0 at 0 ms (restore to centre at 500), button 0 down at 200 ms (restore at 800 to the state at 200: x=0, up).
    const uint64_t id = e.QueueKeys({JoyEntry(X(0), 0, 500), JoyEntry(Button0(true), 200, 600)});
    REQUIRE(e.RunFrames(60));   // ~1 s: both restores are done
    const auto r = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(r.has_value());
    CHECK(!r->discarded);

    // The later restore re-applied the earlier move: x stays 0 although the first entry had ended.
    auto check = [&](const std::string& expr, const std::string& expected) {
        e.QueueText("PRINT " + expr + "\r", 0);
        REQUIRE(e.RunFrames(60));
        const auto rows = Rows(e);
        INFO("text page 1 (trimmed):" << Screen(rows));
        CHECK(RowAfterLast(rows, "]PRINT " + expr) == expected);
    };
    check("PDL(0)", "0");
    check("PEEK(-16287)>127", "0");
    e.QueueJoystick(Centred(), 0);
    CHECK(e.RunFrames(1));
}

TEST_CASE("press_keys: joystick entries only")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));
    (void)e.TakeAppliedInputs();

    const uint64_t id = e.QueueKeys({JoyEntry(X(0), 0, 100)});
    REQUIRE(e.RunFrames(3));   // ~50 ms
    CHECK(!FindApplied(e.TakeAppliedInputs(), id).has_value());
    REQUIRE(e.RunFrames(5));
    const auto r = FindApplied(e.TakeAppliedInputs(), id);
    REQUIRE(r.has_value());
    CHECK(!r->discarded);
    REQUIRE(r->keys.size() == 1);
    CHECK(r->keys[0].joystick);
    CHECK(r->lastCycle - r->firstCycle >= MsToCycles(100));
}

namespace {
// A 6502 loop like a slowly polling title screen: it reads $C000 once every 92,240 cycles (about 90.4 ms) and never
// clears the strobe. LDA $C000; LDX #72; L: LDY #255; M: DEY; BNE M; DEX; BNE L; JMP $0300 (D10, "Read-anchored
// delays").
constexpr uint64_t kPollPeriod = 92240;

void StartPollLoop(Emulator& e)
{
    const uint8_t code[] = {173, 0, 192, 162, 72, 160, 255, 136, 208, 253, 202, 208, 248, 76, 0, 3};
    std::string pokes;
    for (size_t i = 0; i < sizeof(code); ++i)
        pokes += (i ? ":POKE " : "POKE ") + std::to_string(768 + i) + "," + std::to_string(code[i]);
    e.QueueText(pokes + "\r", 0);
    REQUIRE(e.RunFrames(120));
    e.QueueText("CALL 768\r", 0);
    REQUIRE(e.RunFrames(60));
    (void)e.TakeAppliedInputs();
}

SequenceKey Key(char c, uint32_t delayMs, bool delayFromRead = false)
{
    SequenceKey k;
    k.key = KeySpec(uint8_t(c));
    k.delayMs = delayMs;
    k.holdMs = 0;
    k.delayFromRead = delayFromRead;
    return k;
}

// Queues the sequence and runs until it completes.
InputApplied RunSequence(Emulator& e, std::vector<SequenceKey> keys)
{
    const uint64_t id = e.QueueKeys(std::move(keys));
    for (int i = 0; i < 120; ++i) {
        REQUIRE(e.RunFrames(5));
        if (const auto a = FindApplied(e.TakeAppliedInputs(), id))
            return *a;
    }
    FAIL("the sequence did not complete");
    return {};
}
}  // namespace

TEST_CASE("press_keys: a key's read cycle is the cycle of the program's read")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartPollLoop(e);

    // Each key waits for the read of the one before, so B, C and D are read at consecutive polls: exactly one loop
    // apart. A read rounded to a step boundary (1,020 cycles) could not be.
    const InputApplied a = RunSequence(e, {Key('A', 0), Key('B', 0), Key('C', 0), Key('D', 0)});
    REQUIRE(a.keys.size() == 4);
    CHECK(a.keys[2].readCycle - a.keys[1].readCycle == kPollPeriod);
    CHECK(a.keys[3].readCycle - a.keys[2].readCycle == kPollPeriod);
}

TEST_CASE("press_keys: an entry goes in on its due cycle, not at the next step boundary")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));
    (void)e.TakeAppliedInputs();

    const InputApplied a = RunSequence(e, {JoyEntry(X(10), 0, 0), JoyEntry(X(20), 37, 0)});
    REQUIRE(a.keys.size() == 2);
    const uint64_t gap = a.keys[1].appliedCycle - a.keys[0].appliedCycle;
    CHECK(gap >= MsToCycles(37));
    CHECK(gap < MsToCycles(37) + 8);   // within one instruction
    e.QueueJoystick(Centred(), 0);
    CHECK(e.RunFrames(1));
}

TEST_CASE("press_keys: read-anchored delays keep their timing relative to the program at every phase")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartPollLoop(e);

    std::set<uint64_t> fromApplied, fromRead;
    for (uint32_t offset = 0; offset < 90; offset += 10) {
        INFO("phase offset " << offset << " ms");
        REQUIRE(e.RunCycles(MsToCycles(offset)));
        const InputApplied a = RunSequence(e, {Key('A', 0), Key('B', 200)});
        fromApplied.insert(a.keys[1].readCycle - a.keys[0].readCycle);
        const InputApplied r = RunSequence(e, {Key('A', 0), Key('B', 200, true)});
        fromRead.insert(r.keys[1].readCycle - r.keys[0].readCycle);
        const uint64_t afterRead = r.keys[1].appliedCycle - r.keys[0].readCycle;
        CHECK(afterRead >= MsToCycles(200));
        CHECK(afterRead < MsToCycles(200) + 8);
    }
    CHECK(fromApplied.size() > 1);   // counted from the apply, the gap the program sees depends on the phase
    REQUIRE(fromRead.size() == 1);   // counted from the read, it never does
    CHECK(*fromRead.begin() == 3 * kPollPeriod);   // 200 ms after a poll: the third poll after it
}

TEST_CASE("press_keys: delay_from read with no key before the entry counts as applied")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartPollLoop(e);

    const uint64_t queuedAt = e.EmulatedCycles();
    const InputApplied first = RunSequence(e, {Key('A', 50, true)});
    CHECK(first.keys[0].appliedCycle - queuedAt >= MsToCycles(50));   // the first entry: from the call
    CHECK(first.keys[0].appliedCycle - queuedAt < MsToCycles(50) + 8);

    const InputApplied a = RunSequence(e, {JoyEntry(X(10), 0, 0), Key('A', 30, true)});
    const uint64_t gap = a.keys[1].appliedCycle - a.keys[0].appliedCycle;   // no key before it: from the joystick
    CHECK(gap >= MsToCycles(30));
    CHECK(gap < MsToCycles(30) + 8);
    e.QueueJoystick(Centred(), 0);
    CHECK(e.RunFrames(1));
}

TEST_CASE("press_keys: a read-anchored joystick entry counts from the read, without the key guard")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartPollLoop(e);

    SequenceKey joy = JoyEntry(X(10), 100, 0);
    joy.delayFromRead = true;
    const InputApplied a = RunSequence(e, {Key('A', 0), joy});
    REQUIRE(a.keys.size() == 2);
    const uint64_t afterRead = a.keys[1].appliedCycle - a.keys[0].readCycle;
    CHECK(afterRead >= MsToCycles(100));
    CHECK(afterRead < MsToCycles(100) + 8);

    SequenceKey now = JoyEntry(X(20), 0, 0);   // delay 0: at the read (the end of its step), no 1 ms guard
    now.delayFromRead = true;
    const InputApplied b = RunSequence(e, {Key('B', 0), now});
    CHECK(b.keys[1].appliedCycle - b.keys[0].readCycle < MsToCycles(1));
    e.QueueJoystick(Centred(), 0);
    CHECK(e.RunFrames(1));
}

TEST_CASE("press_keys: a sequence ending in a joystick entry completes no earlier than that entry")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartPollLoop(e);

    const InputApplied a = RunSequence(e, {Key('A', 0), JoyEntry(X(10), 500, 0)});
    REQUIRE(a.keys.size() == 2);
    CHECK(a.lastCycle >= a.keys[1].appliedCycle);
    CHECK(a.lastCycle >= a.keys[0].readCycle);
    e.QueueJoystick(Centred(), 0);
    CHECK(e.RunFrames(1));
}

namespace {
// A 6502 program that behaves like a turn-based BASIC game (D10, "Ready input"). Idle, it polls $C000 from two places
// (A at $0300 and B at $0308), 12.5 ms apart. A key it finds there is taken (STA $C010) and starts a 380 ms "move" in
// which it polls from a third place (C, $0324) and ignores any key it finds, as BASIC's Ctrl-C check does. After the
// move for 'P' it asks a question, as BAM does on the staircase: a tight read loop (D, $0338) that takes keys until
// RETURN, then makes another move (the answer's) before it is idle again.
//   idle:  0300 LDA $C000 / BMI take / JSR delay / 0308 LDA $C000 / BMI take / JSR delay / JMP idle
//   take:  0316 STA $C010 / STA $0381 / LDA #30 / STA $0380
//          move: JSR delay / 0324 LDA $C000 / DEC $0380 / BNE move / LDA $0381 / CMP #$D0 / BEQ prompt / JMP idle
//   prompt: 0338 LDA $C000 / BPL prompt / 033D LDA $C000 (the fetch, as the //e ROM's) / STA $C010 / CMP #$8D /
//           BNE prompt / JMP $0319 (a move, RETURN kept)
//   delay: 0360 LDX #10 / LDY #255 / DEY / BNE / DEX / BNE / RTS (about 12.5 ms)
void StartTurnBasedGame(Emulator& e)
{
    const std::vector<std::pair<int, std::vector<uint8_t>>> program = {
        {0x300, {0xAD, 0x00, 0xC0, 0x30, 0x11, 0x20, 0x60, 0x03, 0xAD, 0x00, 0xC0, 0x30, 0x09, 0x20, 0x60, 0x03,
                 0x4C, 0x00, 0x03}},
        {0x316, {0x8D, 0x10, 0xC0, 0x8D, 0x81, 0x03, 0xA9, 0x1E, 0x8D, 0x80, 0x03, 0x20, 0x60, 0x03, 0xAD, 0x00, 0xC0,
                 0xCE, 0x80, 0x03, 0xD0, 0xF5, 0xAD, 0x81, 0x03, 0xC9, 0xD0, 0xF0, 0x05, 0x4C, 0x00, 0x03}},
        {0x338, {0xAD, 0x00, 0xC0, 0x10, 0xFB, 0xAD, 0x00, 0xC0, 0x8D, 0x10, 0xC0, 0xC9, 0x8D, 0xD0, 0xF1, 0x4C, 0x19,
                 0x03}},
        {0x360, {0xA2, 0x0A, 0xA0, 0xFF, 0x88, 0xD0, 0xFD, 0xCA, 0xD0, 0xF8, 0x60}},
    };
    std::vector<std::string> pokes;
    for (const auto& [address, bytes] : program)
        for (size_t i = 0; i < bytes.size(); ++i)
            pokes.push_back("POKE " + std::to_string(address + int(i)) + "," + std::to_string(bytes[i]));
    for (size_t i = 0; i < pokes.size(); i += 12) {
        std::string line;
        for (size_t j = i; j < (std::min)(pokes.size(), i + 12); ++j)
            line += (line.empty() ? "" : ":") + pokes[j];
        e.QueueText(line + "\r", 0);
        REQUIRE(e.RunFrames(120));
    }
    e.QueueText("CALL 768\r", 0);
    REQUIRE(e.RunFrames(60));
    (void)e.TakeAppliedInputs();
}

// Queues the sequence with wait_for_ready (and until_ready) and runs until it is reported (completed or not); within
// `frames` frames.
InputApplied RunReadySequence(Emulator& e, std::vector<SequenceKey> keys, bool untilReady = false, int frames = 1500)
{
    const uint64_t id = e.QueueKeys(std::move(keys), true, untilReady);
    for (int i = 0; i < frames / 5; ++i) {
        REQUIRE(e.RunFrames(5));
        if (const auto a = FindApplied(e.TakeAppliedInputs(), id))
            return *a;
    }
    FAIL("the sequence was not reported");
    return {};
}
}  // namespace

TEST_CASE("press_keys wait_for_ready: a key goes in only when the program is idle again, which wait_for_read does not do")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartTurnBasedGame(e);

    // A move takes about 380 ms. The next key goes in about a millisecond after the previous one is read, in the middle
    // of the move: it is not lost here only because this program keeps it, a BASIC game replaces it.
    const InputApplied plain = RunSequence(e, {Key('A', 0), Key('A', 0)});
    REQUIRE(plain.keys.size() == 2);
    CHECK(plain.keys[1].appliedCycle - plain.keys[0].appliedCycle < MsToCycles(30));
    REQUIRE(e.RunFrames(240));   // the moves finish
    (void)e.TakeAppliedInputs();

    const InputApplied ready = RunReadySequence(e, {Key('A', 0), Key('A', 0), Key('A', 0)});
    CHECK(!ready.discarded);
    CHECK(ready.stopped == InputStop::None);
    REQUIRE(ready.keys.size() == 3);
    for (size_t i = 0; i < ready.keys.size(); ++i) {
        CHECK(ready.keys[i].readCycle >= ready.keys[i].appliedCycle);
        if (i > 0) {   // each key waited for the whole move, then went in right after the program was idle
            CHECK(ready.keys[i].appliedCycle - ready.keys[i - 1].appliedCycle >= MsToCycles(380));
            CHECK(ready.keys[i].appliedCycle - ready.keys[i - 1].appliedCycle < MsToCycles(380 + 60));
        }
    }
}

TEST_CASE("press_keys wait_for_ready: the sequence stops when the program starts a prompt, before any stray key")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartTurnBasedGame(e);

    const uint64_t id = e.QueueKeys({Key('A', 0), Key('P', 0), Key('A', 0), Key('A', 0)}, true);
    std::optional<InputApplied> stopped;
    for (int i = 0; i < 300 && !stopped; ++i) {   // one frame at a time, so the answer below follows the stop at once
        REQUIRE(e.RunFrames(1));
        stopped = FindApplied(e.TakeAppliedInputs(), id);
    }
    REQUIRE(stopped.has_value());
    const InputApplied a = *stopped;
    CHECK(a.discarded);
    CHECK(a.stopped == InputStop::InputChanged);
    REQUIRE(a.keys.size() == 2);   // A and P were pressed, the two after the prompt appeared were not
    CHECK(a.keys[1].readCycle > 0);

    // The prompt is still waiting for its key: nothing was typed into it, and a sequence begun here, at once, may
    // answer it. The place the move polled from, just before the prompt began, is not an idle place here.
    const uint64_t answerId = e.QueueKeys({Key('Y', 0), Key('\r', 0)}, true);
    REQUIRE(e.RunFrames(2));
    const auto answer = FindApplied(e.TakeAppliedInputs(), answerId);
    REQUIRE(answer.has_value());
    CHECK(!answer->discarded);
    REQUIRE(answer->keys.size() == 2);
    CHECK(answer->keys[1].appliedCycle - answer->keys[0].appliedCycle < MsToCycles(2));   // the prompt was ready at once

    // Moves sent straight after the answer, while the program is still busy with it: the first waits until the idle
    // loop takes it, and that loop is what the call learns, not the prompt's.
    const InputApplied moves = RunReadySequence(e, {Key('A', 0), Key('A', 0)});
    CHECK(!moves.discarded);
    CHECK(moves.keys.size() == 2);
}

TEST_CASE("press_keys wait_for_ready: a program that is never ready again drops the rest after 600 frames")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartTurnBasedGame(e);

    e.QueueKey(KeySpec(uint8_t('P')), 0);   // a move, then the prompt
    REQUIRE(e.RunFrames(45));
    (void)e.TakeAppliedInputs();

    // The idle place learned at the first key is the prompt's own loop, which the program leaves once it has RETURN.
    const InputApplied a = RunReadySequence(e, {Key('\r', 0), Key('A', 0)});
    CHECK(a.discarded);
    CHECK(a.stopped == InputStop::NotReady);
    REQUIRE(a.keys.size() == 1);
    CHECK(a.lastCycle == 0);
}

TEST_CASE("press_keys until_ready: the sequence completes once the program is ready again after the last key")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartTurnBasedGame(e);

    // Without it the sequence completes when the program takes the last key, at the start of its 380 ms move.
    const InputApplied taken = RunReadySequence(e, {Key('A', 0)});
    REQUIRE(taken.keys.size() == 1);
    CHECK(taken.lastCycle - taken.keys[0].appliedCycle < MsToCycles(60));
    CHECK(!taken.then.has_value());
    REQUIRE(e.RunFrames(60));   // the move finishes
    (void)e.TakeAppliedInputs();

    const InputApplied ready = RunReadySequence(e, {Key('A', 0), Key('A', 0)}, true);
    CHECK(!ready.discarded);
    REQUIRE(ready.then.has_value());
    CHECK(*ready.then == InputStop::None);
    REQUIRE(ready.keys.size() == 2);
    const uint64_t after = ready.lastCycle - ready.keys[1].appliedCycle;   // the whole move, then idle again
    CHECK(after >= MsToCycles(380));
    CHECK(after < MsToCycles(380 + 60));
}

TEST_CASE("press_keys until_ready: a last key that brings up a prompt completes, reporting the other kind of input")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartTurnBasedGame(e);

    const InputApplied a = RunReadySequence(e, {Key('A', 0), Key('P', 0)}, true);
    CHECK(!a.discarded);   // every key was pressed
    REQUIRE(a.then.has_value());
    CHECK(*a.then == InputStop::InputChanged);
    REQUIRE(a.keys.size() == 2);
    CHECK(a.lastCycle - a.keys[1].appliedCycle >= MsToCycles(380));   // when the prompt began, after the move

    // The prompt still waits for its answer.
    const InputApplied answer = RunReadySequence(e, {Key('Y', 0), Key('\r', 0)});
    CHECK(!answer.discarded);
    REQUIRE(answer.keys.size() == 2);
    CHECK(answer.keys[1].appliedCycle - answer.keys[0].appliedCycle < MsToCycles(2));
}

TEST_CASE("press_keys until_ready: a program that is not ready again within 600 frames completes, not ready")
{
    auto& e = StartedEmulator();
    DosBasic dos(e);
    REQUIRE(dos.Booted());
    StartTurnBasedGame(e);

    e.QueueKey(KeySpec(uint8_t('P')), 0);   // a move, then the prompt
    REQUIRE(e.RunFrames(45));
    (void)e.TakeAppliedInputs();

    // The answer: the idle place learned at its key is the prompt's own loop, which the program does not come back to.
    const InputApplied a = RunReadySequence(e, {Key('\r', 0)}, true);
    CHECK(!a.discarded);
    REQUIRE(a.then.has_value());
    CHECK(*a.then == InputStop::NotReady);
    REQUIRE(a.keys.size() == 1);
    CHECK(a.keys[0].readCycle > 0);
    CHECK(a.lastCycle - a.keys[0].appliedCycle >= MsToCycles(9900));   // 600 frames
}

TEST_CASE("press_keys wait_for_ready: only plain keys that wait for the program are allowed")
{
    auto& e = StartedEmulator();
    PowerCycleOnExit restore{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(2));

    SequenceKey delayed = Key('A', 0);
    delayed.delayMs = 10;
    SequenceKey unwaiting = Key('A', 0);
    unwaiting.waitForRead = false;
    SequenceKey fromRead = Key('A', 0, true);
    CHECK_THROWS_AS(e.QueueKeys({Key('A', 0), delayed}, true), std::invalid_argument);
    CHECK_THROWS_AS(e.QueueKeys({Key('A', 0), unwaiting}, true), std::invalid_argument);
    CHECK_THROWS_AS(e.QueueKeys({Key('A', 0), fromRead}, true), std::invalid_argument);
    CHECK_THROWS_AS(e.QueueKeys({Key('A', 0), JoyEntry(X(10), 0, 0)}, true), std::invalid_argument);
    CHECK_THROWS_AS(e.QueueKeys({Key('A', 0)}, false, true), std::invalid_argument);   // until_ready needs wait_for_ready
    CHECK(e.PendingInputEvents() == 0);
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
