// SPDX-License-Identifier: MIT
#pragma once
// Helpers shared by the in-process M4 checkpoint tests (snapshot_test.cpp, zz_restore_failure_test.cpp).
#include "doctest/doctest.h"
#include "host/emulator.h"
#include "core/working_copy.h"
#include "support/paths.h"
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace harness::testsupport {

namespace ckpt_fs = std::filesystem;

// A scratch directory outside %TEMP%\applewin-harness, removed (with everything in it) when the guard ends.
struct ScratchDir {
    ckpt_fs::path path;
    explicit ScratchDir(const std::string& tag)
        : path(ckpt_fs::temp_directory_path() / ("awh-ckpt-" + tag + "-" + std::to_string(GetCurrentProcessId())))
    {
        std::error_code ec;
        ckpt_fs::remove_all(path, ec);
        ckpt_fs::create_directories(path);
    }
    ~ScratchDir()
    {
        std::error_code ec;
        ckpt_fs::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

// Ejects both drives and power-cycles, so the tests that follow start from a clean machine. Never throws.
struct EjectAndPowerCycle {
    Emulator& e;
    ~EjectAndPowerCycle()
    {
        try { e.EjectDisk(0); } catch (...) {}
        try { e.EjectDisk(1); } catch (...) {}
        try { e.PowerCycle(); } catch (...) {}
    }
};

// DOS 3.3 booted in-process from a working copy (D5: the image in the submodule is never opened for writing). Members
// are destroyed in reverse order: the guard ejects before the copy and its directory are removed.
struct DosBoot {
    Emulator& e;
    ScratchDir dir;
    WorkingCopy copy;
    EjectAndPowerCycle guard;
    explicit DosBoot(Emulator& emu, const std::string& tag)
        : e(emu), dir(tag), copy(DosMasterImage(), dir.path / "disk"), guard{emu}
    {
        e.InsertDisk(0, copy.Path());
        e.PowerCycle();
        REQUIRE(e.RunFrames(720));
    }
};

inline std::vector<uint8_t> ReadAll(const ckpt_fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline void WriteAll(const ckpt_fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

}  // namespace harness::testsupport
