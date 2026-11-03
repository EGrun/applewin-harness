// SPDX-License-Identifier: MIT
// Persistent checkpoint store and the build stamp (spec: docs/specs/persistence.md 3.1-3.2; D18). Test prefix "persist:".
// Plain files stand in for disk images and AppleWin's save state; no emulator is involved.
#include "doctest/doctest.h"
#include "host/harness_version.h"
#include "server/checkpoint_store.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <string>

using namespace harness;
namespace fs = std::filesystem;

namespace {

// A fresh directory per test, removed afterwards.
struct TempDir {
    fs::path path;
    TempDir()
    {
        wchar_t buf[MAX_PATH + 1];
        GetTempPathW(MAX_PATH, buf);
        static int counter = 0;
        path = fs::path(buf) / ("harness-persist-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void WriteFile(const fs::path& p, const std::string& bytes)
{
    std::ofstream(p, std::ios::binary | std::ios::trunc) << bytes;
}

std::string ReadFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

SnapshotMeta MakeMeta()
{
    SnapshotMeta m;
    m.emulatedCycles = 123456789;
    m.powerOnOrigin = 4242;
    m.speed = SpeedSettings{16, true, true};
    m.joystickX = 10;
    m.joystickY = 250;
    return m;
}

// Two plain-file "disks" under dir, returned as DiskSources.
std::vector<CheckpointStore::DiskSource> MakeDisks(const fs::path& dir)
{
    WriteFile(dir / "work1.dsk", "disk one bytes");
    WriteFile(dir / "work2.dsk", "disk two bytes, longer");
    return {{0, dir / "work1.dsk", dir / "orig1.dsk", "C:\\short\\orig1.dsk"},
            {1, dir / "work2.dsk", dir / "orig2.dsk", ""}};
}

CheckpointStore::Entry Make(CheckpointStore& s, const std::string& name, const fs::path& scratch,
                            const std::string& runMode = "stepped")
{
    WriteFile(s.PendingStateFile(), "state: yaml");
    return s.Create(name, MakeMeta(), MakeDisks(scratch), runMode);
}

}  // namespace

TEST_CASE("persist: BuildId is the version, a plus and twelve hex digits or unknown")
{
    const std::string b = BuildId();
    CHECK(std::regex_match(b, std::regex(R"(\d+\.\d+\.\d+\+([0-9a-f]{12}|unknown))")));
    CHECK(b.rfind(std::string(Version()) + "+", 0) == 0);
}

TEST_CASE("persist: a checkpoint is read back by a second store on the same directory")
{
    TempDir t;
    const fs::path dir = t.path / "states";
    const fs::path scratch = t.path / "scratch";
    fs::create_directories(scratch);
    CheckpointStore::Entry made;
    {
        CheckpointStore a(t.path / "work", dir);
        made = Make(a, "first", scratch);
        CHECK_FALSE(made.foreign);
        CHECK_FALSE(a.Get("cp-1").foreign);
    }
    CheckpointStore b(t.path / "work2", dir);
    const auto list = b.List();
    REQUIRE(list.size() == 1);
    const auto& e = list[0];
    CHECK(e.id == made.id);
    CHECK(e.name == "first");
    CHECK(e.createdAt == made.createdAt);
    const SnapshotMeta want = MakeMeta();
    CHECK(e.meta.emulatedCycles == want.emulatedCycles);
    CHECK(e.meta.powerOnOrigin == want.powerOnOrigin);
    CHECK(e.meta.speed.machineSpeed == want.speed.machineSpeed);
    CHECK(e.meta.speed.enhanceDisk == want.speed.enhanceDisk);
    CHECK(e.meta.speed.fullSpeedOverride == want.speed.fullSpeedOverride);
    CHECK(e.meta.joystickX == want.joystickX);
    CHECK(e.meta.joystickY == want.joystickY);
    CHECK(e.build == BuildId());
    CHECK(e.runMode == "stepped");
    CHECK(e.foreign);
    REQUIRE(e.disks.size() == 2);
    CHECK(e.disks[0].drive == 0);
    CHECK(e.disks[1].drive == 1);
    CHECK(e.disks[0].originalPath == scratch / "orig1.dsk");
    CHECK(e.disks[1].originalPath == scratch / "orig2.dsk");
    CHECK(e.disks[0].statePath == "C:\\short\\orig1.dsk");
    CHECK(e.disks[1].statePath == "");
    CHECK(ReadFile(e.disks[0].copy) == "disk one bytes");
    CHECK(ReadFile(e.disks[1].copy) == "disk two bytes, longer");
    CHECK(ReadFile(e.StateFile()) == "state: yaml");
    CHECK(b.Skipped().empty());
}

TEST_CASE("persist: ids continue past the highest cp directory, complete or not")
{
    TempDir t;
    const fs::path dir = t.path / "states";
    const fs::path scratch = t.path / "scratch";
    fs::create_directories(scratch);
    {
        CheckpointStore a(t.path / "work", dir);
        Make(a, "a", scratch);
        Make(a, "b", scratch);
        Make(a, "c", scratch);
        a.Delete("cp-1");
        a.Delete("cp-2");
    }
    fs::create_directories(dir / "cp-7");
    WriteFile(dir / "cp-7" / "meta.json", "{ not json");
    CheckpointStore s(t.path / "work2", dir);
    REQUIRE(s.List().size() == 1);
    CHECK(s.List()[0].id == "cp-3");
    REQUIRE(s.Skipped().size() == 1);
    CHECK(s.Skipped()[0].find("cp-7") != std::string::npos);
    CHECK(fs::is_directory(dir / "cp-7"));
    CHECK(Make(s, "next", scratch).id == "cp-8");
}

TEST_CASE("persist: leftover tmp directories and pending.yaml are deleted at open")
{
    TempDir t;
    const fs::path dir = t.path / "states";
    fs::create_directories(dir / "cp-9.tmp");
    WriteFile(dir / "cp-9.tmp" / "state.yaml", "x");
    WriteFile(dir / "pending.yaml", "x");
    CheckpointStore s(t.path / "work", dir);
    CHECK_FALSE(fs::exists(dir / "cp-9.tmp"));
    CHECK_FALSE(fs::exists(dir / "pending.yaml"));
    const fs::path scratch = t.path / "scratch";
    fs::create_directories(scratch);
    CHECK(Make(s, "", scratch).id == "cp-1");
}

TEST_CASE("persist: a second store on a locked directory throws CheckpointDirLocked")
{
    TempDir t;
    const fs::path dir = t.path / "states";
    auto a = std::make_unique<CheckpointStore>(t.path / "work", dir);
    CHECK_THROWS_AS(CheckpointStore(t.path / "work2", dir), CheckpointDirLocked);
    a.reset();
    CHECK_NOTHROW(CheckpointStore(t.path / "work2", dir));
}

TEST_CASE("persist: a failed Create leaves no directory and does not use up an id")
{
    TempDir t;
    const fs::path dir = t.path / "states";
    const fs::path scratch = t.path / "scratch";
    fs::create_directories(scratch);
    CheckpointStore s(t.path / "work", dir);
    WriteFile(s.PendingStateFile(), "state: yaml");
    const std::vector<CheckpointStore::DiskSource> bad = {{0, scratch / "missing.dsk", scratch / "o.dsk", ""}};
    CHECK_THROWS_AS(s.Create("x", MakeMeta(), bad, "stepped"), std::runtime_error);
    CHECK_FALSE(fs::exists(dir / "cp-1"));
    CHECK_FALSE(fs::exists(dir / "cp-1.tmp"));
    CHECK(Make(s, "ok", scratch).id == "cp-1");
}

TEST_CASE("persist: Delete removes the directory and the limit counts entries read at open")
{
    TempDir t;
    const fs::path dir = t.path / "states";
    const fs::path scratch = t.path / "scratch";
    fs::create_directories(scratch);
    {
        CheckpointStore a(t.path / "work", dir);
        Make(a, "one", scratch);
        a.Delete("cp-1");
        CHECK_FALSE(fs::exists(dir / "cp-1"));
        CHECK(a.List().empty());
    }
    {
        CheckpointStore a(t.path / "work", dir);
        for (size_t i = 0; i < CheckpointStore::kMaxCheckpoints; ++i) Make(a, "", scratch);
    }
    CheckpointStore b(t.path / "work2", dir);
    REQUIRE(b.List().size() == CheckpointStore::kMaxCheckpoints);
    WriteFile(b.PendingStateFile(), "state: yaml");
    try {
        b.Create("over", MakeMeta(), MakeDisks(scratch), "stepped");
        FAIL("Create past the limit did not throw");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("256") != std::string::npos);
    }
}

TEST_CASE("persist: the ephemeral store records build, run mode and paths too")
{
    TempDir t;
    const fs::path scratch = t.path / "scratch";
    fs::create_directories(scratch);
    CheckpointStore s(t.path / "work");
    const auto e = Make(s, "eph", scratch, "realtime");
    CHECK(e.build == BuildId());
    CHECK(e.runMode == "realtime");
    CHECK_FALSE(e.foreign);
    REQUIRE(e.disks.size() == 2);
    CHECK(e.disks[0].originalPath == scratch / "orig1.dsk");
    CHECK(e.disks[0].statePath == "C:\\short\\orig1.dsk");
    CHECK(e.dir == t.path / "work" / "checkpoints" / "cp-1");
}
