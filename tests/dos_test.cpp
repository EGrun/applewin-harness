// DOS 3.3 in-process golden (spec: docs/specs/m2.md, pass conditions 1 and 3).
#include "doctest/doctest.h"
#include "host/emulator.h"
#include "core/sha256.h"
#include "core/working_copy.h"
#include "support/emulator_fixture.h"
#include "support/golden.h"
#include "support/paths.h"
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace harness;
using namespace harness::testsupport;

// doctest's default order is by file name, then line. This is the first file that starts the shared emulator, so this
// case (moved here from emulator_test.cpp) runs before any Start.
TEST_CASE("emu: precondition")
{
    CHECK_THROWS_AS(Emulator::Instance().RunFrames(1), std::logic_error);
}

namespace {

// Strips spaces and the enhanced //e 40-column cursor (a flashing checkerboard: 0xFF on screen, 0x7F after & 0x7F).
std::string TrimRow(const std::string& s)
{
    auto blank = [](char c) { return c == ' ' || c == '\x7f'; };
    size_t b = 0, e = s.size();
    while (b < e && blank(s[b])) ++b;
    while (e > b && blank(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Ejects the disk and power-cycles, so the M1 boot tests (no disk) that follow stay valid. Never throws.
struct EjectAndPowerCycle {
    Emulator& e;
    ~EjectAndPowerCycle()
    {
        try { e.EjectDisk(0); } catch (...) {}
        try { e.PowerCycle(); } catch (...) {}
    }
};

}  // namespace

TEST_CASE("dos: PRINT 2+2 in-process")
{
    auto& e = StartedEmulator();
    // Boot from a working copy (D5) so the image in the AppleWin submodule is never opened for writing. It lives outside
    // %TEMP%\applewin-harness and is removed with its directory when the test ends.
    const auto work = std::filesystem::temp_directory_path() / ("awh-dos-test-" + std::to_string(GetCurrentProcessId()));
    WorkingCopy copy(DosMasterImage(), work);
    e.InsertDisk(0, copy.Path());
    EjectAndPowerCycle restore{e};   // declared after copy: ejects before the copy is deleted

    e.PowerCycle();
    REQUIRE(e.RunFrames(600));
    REQUIRE(e.TypeText("PRINT 2+2\r"));
    REQUIRE(e.RunFrames(60));

    std::vector<std::string> rows;
    for (int r = 0; r < 24; ++r)
        rows.push_back(TrimRow(e.ReadTextRow(r)));
    bool found = false;
    for (size_t r = 0; r + 2 < rows.size() && !found; ++r)
        found = rows[r] == "]PRINT 2+2" && rows[r + 1] == "4" && rows[r + 2] == "]";
    std::string screen;
    for (const auto& row : rows) screen += "\n|" + row;
    INFO("text page 1 (trimmed):" << screen);
    CHECK(found);

    const std::string golden = ReadGoldenTrimmed("m2_dos_print.sha256");
    REQUIRE_MESSAGE(!golden.empty(), "golden m2_dos_print.sha256 missing");
    CHECK(FrameHash(e.Frame()) == golden);
}
