// SPDX-License-Identifier: MIT
// M4 restore error paths in-process (spec: docs/specs/m4.md "Restore purity"; plan Review Focus 1 and 3). Test prefix
// "ckpt:". Named zz_ so doctest (file-name order) runs it after every other in-process test: a failed load leaves AppleWin
// in a post-failure state, and each case ends by re-inserting a fresh working copy and power-cycling (then ejecting).
#include "doctest/doctest.h"
#include "host/emulator.h"
#include "core/working_copy.h"
#include "support/emulator_fixture.h"
#include "support/paths.h"
#include "support/snapshot_support.h"
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace harness;
using namespace harness::testsupport;

namespace {

// After a failed load: boot a fresh working copy (InsertDisk + PowerCycle), check the machine runs, then eject. Never
// throws out of the test's scope guard.
void RecoverWithFreshDisk(Emulator& e, const fs::path& dir)
{
    WorkingCopy fresh(DosMasterImage(), dir / "fresh");
    try { e.EjectDisk(0); } catch (...) {}
    e.InsertDisk(0, fresh.Path());
    e.PowerCycle();
    CHECK(e.RunFrames(2));
    e.EjectDisk(0);
    e.PowerCycle();
}

}  // namespace

// ---- Restore error paths (plan Review Focus 1 and 3) ----

TEST_CASE("ckpt: LoadSnapshot with a disk image missing throws and opens no dialog")
{
    auto& e = StartedEmulator();
    DosBoot boot(e, "missing");
    const fs::path state = boot.dir.path / "state.yaml";
    const SnapshotMeta meta = e.SaveSnapshot(state);
    e.EjectAll();
    REQUIRE(fs::remove(boot.copy.Path()));   // the image the save state names is gone
    CHECK_THROWS_AS(e.LoadSnapshot(state, meta), std::runtime_error);
    RecoverWithFreshDisk(e, boot.dir.path);
}

TEST_CASE("ckpt: LoadSnapshot of a corrupt state file throws with AppleWin's message")
{
    auto& e = StartedEmulator();
    DosBoot boot(e, "corrupt");
    const fs::path state = boot.dir.path / "state.yaml";
    const SnapshotMeta meta = e.SaveSnapshot(state);
    // Keep the header (so the load gets past ParseFileHdr and has started replacing state), then garbage.
    std::vector<uint8_t> bytes = ReadAll(state);
    REQUIRE(bytes.size() > 1000);
    bytes.resize(bytes.size() * 2 / 5);
    const std::string junk = "\n  ]]] not: [yaml\n";
    bytes.insert(bytes.end(), junk.begin(), junk.end());
    WriteAll(state, bytes);

    const size_t messagesBefore = e.Messages().size();
    e.EjectAll();
    std::string what;
    try {
        e.LoadSnapshot(state, meta);
    } catch (const std::runtime_error& ex) {
        what = ex.what();
    }
    REQUIRE_MESSAGE(!what.empty(), "LoadSnapshot of a corrupt file did not throw std::runtime_error");
    REQUIRE(e.Messages().size() > messagesBefore);
    CHECK(what.find(e.Messages().back()) != std::string::npos);

    RecoverWithFreshDisk(e, boot.dir.path);   // the machine is usable again after a fresh boot
}

