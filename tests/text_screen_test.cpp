// M5 pass condition 2 and Review Focus 4 (docs/specs/m5.md "Harness change: get_text_screen"): Emulator::ReadTextScreen
// against known bytes in the text pages and soft switches set by the 6502 itself.
//
// How the switches are set: a tiny machine-language routine (STA $C0xx ... JMP *) is written to $0300, regs.pc is
// pointed at it and the emulator runs one frame, so every switch goes through AppleWin's real I/O write path, exactly
// as a program's STA would. The text bytes are poked straight into the pages (MemGetMainPtr / MemGetAuxPtr) BEFORE the
// final switch routine, while 80STORE and PAGE2 are off and the pointers reach plain main / aux RAM.
// Every test starts from and ends with a power cycle, so the shared emulator is left as other test files expect it.
#include "doctest/doctest.h"
#include "host/emulator.h"
#include "support/emulator_fixture.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#pragma warning(push, 3)
#include "CPU.h"
#include "Memory.h"
#pragma warning(pop)

using namespace harness;
using namespace harness::testsupport;

namespace {

constexpr WORD kRowBase = 0x400;

WORD RowAddr(WORD page, int row) { return WORD(page + 0x80 * (row % 8) + 0x28 * (row / 8)); }

// Runs `STA $C0xx` for each address, then spins on a JMP to itself, for one frame.
void WriteSwitches(Emulator& e, std::initializer_list<WORD> addrs)
{
    std::vector<BYTE> code;
    for (WORD a : addrs) {
        code.push_back(0x8D);
        code.push_back(BYTE(a & 0xFF));
        code.push_back(BYTE(a >> 8));
    }
    const WORD loop = WORD(0x300 + code.size());
    code.push_back(0x4C);
    code.push_back(BYTE(loop & 0xFF));
    code.push_back(BYTE(loop >> 8));
    std::memcpy(MemGetMainPtr(0x300), code.data(), code.size());
    regs.pc = 0x300;
    e.RunFrames(1);
}

// Everything off: 40 columns, primary character set, text mode, no mixed, page 1, lo-res, 80STORE off.
void ResetSwitches(Emulator& e)
{
    WriteSwitches(e, {0xC000, 0xC00C, 0xC00E, 0xC051, 0xC052, 0xC054, 0xC056});
}

void ClearPages()
{
    for (WORD page : {WORD(0x400), WORD(0x800)}) {
        std::memset(MemGetMainPtr(page), 0xA0, 0x400);
        std::memset(MemGetAuxPtr(page), 0xA0, 0x400);
        std::memset(MemGetBankPtr(0) + page, 0xA0, 0x400);   // backing banks too (see PutRow)
        std::memset(MemGetBankPtr(1) + page, 0xA0, 0x400);
    }
}

// Normal-video text (high bit set) at the start of `row` in the page (main or aux).
void PutRow(WORD page, int row, const std::string& text, bool aux = false)
{
    const WORD addr = RowAddr(page, row);
    BYTE* p = aux ? MemGetAuxPtr(addr) : MemGetMainPtr(addr);
    // Also write the backing bank: a poke through MemGetMainPtr lands in AppleWin's paged 'mem' copy without marking the
    // page dirty, so it would be lost when 80STORE + PAGE2 repoints the page at aux RAM.
    BYTE* bank = MemGetBankPtr(aux ? 1 : 0) + addr;
    for (size_t i = 0; i < text.size(); ++i) p[i] = bank[i] = BYTE(text[i] | 0x80);
}

std::string Row(const std::string& text, size_t width = 40)
{
    std::string s = text;
    s.resize(width, ' ');
    return s;
}

std::string RowLabel(const char* prefix, int row) { return std::string(prefix) + std::to_string(row); }

struct CleanMachine {
    Emulator& e;
    CleanMachine() : e(StartedEmulator())
    {
        e.PowerCycle();
        e.RunFrames(2);
        ResetSwitches(e);
        ClearPages();
    }
    ~CleanMachine() { e.PowerCycle(); }
};

}  // namespace

TEST_CASE("text: 40col page 1 decodes all 24 rows")
{
    CleanMachine m;
    for (int r = 0; r < 24; ++r) PutRow(0x400, r, RowLabel("P1 ROW ", r));
    PutRow(0x800, 0, "PAGE TWO");   // must not appear
    const TextScreen s = m.e.ReadTextScreen();
    CHECK(s.mode == "40col");
    for (int r = 0; r < 24; ++r) CHECK(s.lines[size_t(r)] == Row(RowLabel("P1 ROW ", r)));
    CHECK(s.emulatedCycles == m.e.EmulatedCycles());
}

TEST_CASE("text: 40col page 2 reads page $0800")
{
    CleanMachine m;
    for (int r = 0; r < 24; ++r) {
        PutRow(0x400, r, "WRONG PAGE");
        PutRow(0x800, r, RowLabel("P2 ROW ", r));
    }
    WriteSwitches(m.e, {0xC055});   // PAGE2 on (80STORE off)
    const TextScreen s = m.e.ReadTextScreen();
    CHECK(s.mode == "40col");
    for (int r = 0; r < 24; ++r) CHECK(s.lines[size_t(r)] == Row(RowLabel("P2 ROW ", r)));
}

TEST_CASE("text: 80col interleaves aux (even columns) and main (odd columns)")
{
    CleanMachine m;
    const std::string line = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz!@#$%^&*";
    REQUIRE(line.size() == 80);
    const int rows[] = {0, 7, 8, 16, 23};
    for (int r : rows) {
        const std::string rotated = line.substr(size_t(r)) + line.substr(0, size_t(r));
        std::string even, odd;
        for (size_t i = 0; i < 80; i += 2) {
            even += rotated[i];
            odd += rotated[i + 1];
        }
        PutRow(0x400, r, even, true);
        PutRow(0x400, r, odd, false);
    }
    WriteSwitches(m.e, {0xC00D});   // 80COL on
    const TextScreen s = m.e.ReadTextScreen();
    CHECK(s.mode == "80col");
    for (int r : rows) CHECK(s.lines[size_t(r)] == line.substr(size_t(r)) + line.substr(0, size_t(r)));
    CHECK(s.lines[1] == std::string(80, ' '));
}

TEST_CASE("text: mixed40 leaves lines 0-19 empty and decodes the bottom four")
{
    CleanMachine m;
    for (int r = 0; r < 24; ++r) PutRow(0x400, r, RowLabel("ROW ", r));
    WriteSwitches(m.e, {0xC050, 0xC053});   // graphics + mixed (lo-res)
    const TextScreen s = m.e.ReadTextScreen();
    CHECK(s.mode == "mixed40");
    for (int r = 0; r < 20; ++r) CHECK(s.lines[size_t(r)] == "");
    for (int r = 20; r < 24; ++r) CHECK(s.lines[size_t(r)] == Row(RowLabel("ROW ", r)));
}

TEST_CASE("text: mixed40 with hi-res and page 2")
{
    CleanMachine m;
    for (int r = 20; r < 24; ++r) {
        PutRow(0x400, r, "WRONG");
        PutRow(0x800, r, RowLabel("P2 ", r));
    }
    WriteSwitches(m.e, {0xC050, 0xC053, 0xC057, 0xC055});
    const TextScreen s = m.e.ReadTextScreen();
    CHECK(s.mode == "mixed40");
    for (int r = 0; r < 20; ++r) CHECK(s.lines[size_t(r)] == "");
    for (int r = 20; r < 24; ++r) CHECK(s.lines[size_t(r)] == Row(RowLabel("P2 ", r)));
}

TEST_CASE("text: mixed80 interleaves the bottom four lines")
{
    CleanMachine m;
    for (int r = 20; r < 24; ++r) {
        PutRow(0x400, r, "EVEN", true);    // aux: columns 0, 2, 4, ...
        PutRow(0x400, r, "odd!", false);   // main: columns 1, 3, 5, ...
    }
    WriteSwitches(m.e, {0xC00D, 0xC050, 0xC053});   // 80COL, graphics, mixed
    const TextScreen s = m.e.ReadTextScreen();
    CHECK(s.mode == "mixed80");
    for (int r = 0; r < 20; ++r) CHECK(s.lines[size_t(r)] == "");
    for (int r = 20; r < 24; ++r) CHECK(s.lines[size_t(r)] == Row("EoVdEdN!", 80));
}

TEST_CASE("text: graphics mode has 24 empty lines")
{
    CleanMachine m;
    for (int r = 0; r < 24; ++r) PutRow(0x400, r, "HIDDEN");
    for (WORD hires : {WORD(0xC056), WORD(0xC057)}) {
        WriteSwitches(m.e, {0xC050, 0xC052, hires});   // graphics, mixed off
        const TextScreen s = m.e.ReadTextScreen();
        CHECK(s.mode == "graphics");
        for (int r = 0; r < 24; ++r) CHECK(s.lines[size_t(r)] == "");
    }
    WriteSwitches(m.e, {0xC00D});   // 80COL does not make graphics text
    CHECK(m.e.ReadTextScreen().mode == "graphics");
}

TEST_CASE("text: 80STORE with PAGE2 still displays page 1")
{
    CleanMachine m;
    PutRow(0x400, 0, "PAGE ONE");
    PutRow(0x800, 0, "PAGE TWO");
    WriteSwitches(m.e, {0xC001, 0xC055});   // 80STORE on: PAGE2 then selects aux memory, not the displayed page
    const TextScreen s = m.e.ReadTextScreen();
    CHECK(s.mode == "40col");
    CHECK(s.lines[0] == Row("PAGE ONE"));
}

namespace {
struct Glyph { BYTE code; char expected; };

void CheckGlyphs(Emulator& e, const std::vector<Glyph>& glyphs)
{
    BYTE* p = MemGetMainPtr(kRowBase);
    std::string expected;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        p[i] = glyphs[i].code;
        expected += glyphs[i].expected;
    }
    const TextScreen s = e.ReadTextScreen();
    CHECK(s.mode == "40col");
    CHECK(s.lines[0] == Row(expected));
    for (char c : s.lines[0]) CHECK((c >= 0x20 && c <= 0x7E));
}
}  // namespace

TEST_CASE("text: primary character set maps inverse and flashing to plain ASCII")
{
    CleanMachine m;
    CheckGlyphs(m.e, {
        {0x00, '@'}, {0x01, 'A'}, {0x1A, 'Z'}, {0x1F, '_'},          // inverse uppercase
        {0x20, ' '}, {0x31, '1'}, {0x3F, '?'},                        // inverse punctuation and digits
        {0x40, '@'}, {0x41, 'A'}, {0x5A, 'Z'}, {0x5F, '_'},          // flashing uppercase
        {0x60, ' '}, {0x71, '1'}, {0x7F, '?'},                        // flashing punctuation and digits
        {0xA0, ' '}, {0xB1, '1'}, {0xBF, '?'},                        // normal punctuation and digits
        {0xC0, '@'}, {0xC1, 'A'}, {0xDA, 'Z'}, {0xDF, '_'},          // normal uppercase
    });
}

TEST_CASE("text: alternate character set maps inverse, lowercase and MouseText")
{
    CleanMachine m;
    WriteSwitches(m.e, {0xC00F});   // ALTCHARSET on
    CheckGlyphs(m.e, {
        {0x01, 'A'}, {0x1A, 'Z'}, {0x31, '1'},                        // inverse
        {0x40, ' '}, {0x41, ' '}, {0x5F, ' '},                        // MouseText -> space
        {0x61, 'a'}, {0x7A, 'z'},                                     // inverse lowercase
        {0xA0, ' '}, {0xB1, '1'}, {0xC1, 'A'}, {0xE1, 'a'}, {0xFA, 'z'},   // normal
    });
}

TEST_CASE("text: the character mapping applies in 80 columns too")
{
    CleanMachine m;
    BYTE* aux = MemGetAuxPtr(kRowBase);
    BYTE* main = MemGetMainPtr(kRowBase);
    aux[0] = 0x01;  main[0] = 0x41;    // inverse A (aux), flashing A (main)
    aux[1] = 0xC2;  main[1] = 0x32;    // normal B, inverse 2
    WriteSwitches(m.e, {0xC00D});
    const TextScreen s = m.e.ReadTextScreen();
    CHECK(s.mode == "80col");
    CHECK(s.lines[0] == Row("AAB2", 80));
}
