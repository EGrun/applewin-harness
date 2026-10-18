// SPDX-License-Identifier: MIT
// WorkingCopy unit tests (spec: docs/specs/m2.md, "src/core/working_copy.h").
#include "doctest/doctest.h"
#include "core/working_copy.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using harness::WorkingCopy;

namespace {

// A fresh directory under %TEMP% (outside %TEMP%\applewin-harness), removed with everything in it on destruction.
struct TempDir {
    fs::path path;
    TempDir()
    {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() /
               ("awh-working-copy-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(path, ec))
            SetFileAttributesW(e.path().c_str(), FILE_ATTRIBUTE_NORMAL);   // read-only files block remove_all
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

const std::vector<uint8_t> kBytes = {0x00, 0x01, 0xFE, 0xFF, 'D', 'S', 'K', '\r', '\n', 0x7F};

void WriteBytes(const fs::path& p, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> ReadBytes(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool IsReadOnly(const fs::path& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) != 0;
}

void MakeReadOnly(const fs::path& p)
{
    REQUIRE(SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_READONLY));
}

}  // namespace

TEST_CASE("copy: the copy has the same bytes and is not read-only")
{
    TempDir tmp;
    const fs::path original = tmp.path / "disk image.dsk";
    WriteBytes(original, kBytes);
    MakeReadOnly(original);
    const fs::path work = tmp.path / "work";

    WorkingCopy copy(original, work);
    CHECK(copy.Path() == work / "disk image.dsk");
    REQUIRE(fs::exists(copy.Path()));
    CHECK(ReadBytes(copy.Path()) == kBytes);
    CHECK(!IsReadOnly(copy.Path()));
}

TEST_CASE("copy: the original's bytes and last-write time are unchanged")
{
    TempDir tmp;
    const fs::path original = tmp.path / "disk.dsk";
    WriteBytes(original, kBytes);
    const auto past = fs::last_write_time(original) - std::chrono::hours(24);
    fs::last_write_time(original, past);
    {
        WorkingCopy copy(original, tmp.path / "work");
        WriteBytes(copy.Path(), {1, 2, 3});   // writing the copy must not touch the original
    }
    CHECK(ReadBytes(original) == kBytes);
    CHECK(fs::last_write_time(original) == past);
}

TEST_CASE("copy: the destructor removes the copy and the empty work dir")
{
    TempDir tmp;
    const fs::path original = tmp.path / "disk.dsk";
    WriteBytes(original, kBytes);
    const fs::path work = tmp.path / "work";
    fs::path copied;
    {
        WorkingCopy copy(original, work);
        copied = copy.Path();
        REQUIRE(fs::exists(copied));
    }
    CHECK(!fs::exists(copied));
    CHECK(!fs::exists(work));
    CHECK(fs::exists(original));
}

TEST_CASE("copy: move-assignment deletes the old copy, keeps the new one, and the moved-from deletes nothing")
{
    TempDir tmp;
    const fs::path a = tmp.path / "a.dsk";
    const fs::path b = tmp.path / "b.dsk";
    WriteBytes(a, kBytes);
    WriteBytes(b, kBytes);
    const fs::path workA = tmp.path / "workA";
    const fs::path workB = tmp.path / "workB";
    {
        WorkingCopy first(a, workA);
        WorkingCopy second(b, workB);
        const fs::path firstPath = first.Path();
        const fs::path secondPath = second.Path();
        first = std::move(second);
        CHECK(!fs::exists(firstPath));
        CHECK(!fs::exists(workA));
        CHECK(first.Path() == secondPath);
        CHECK(fs::exists(secondPath));
    }
    CHECK(!fs::exists(workB));
    CHECK(fs::exists(a));
    CHECK(fs::exists(b));
}

TEST_CASE("copy: a missing original throws runtime_error naming the file")
{
    TempDir tmp;
    const fs::path missing = tmp.path / "no-such-image.dsk";
    bool threw = false;
    try {
        WorkingCopy copy(missing, tmp.path / "work");
    } catch (const std::runtime_error& e) {
        threw = true;
        CHECK(std::string(e.what()).find("no-such-image.dsk") != std::string::npos);
    } catch (...) {
    }
    CHECK(threw);
}

TEST_CASE("copy: a directory as original throws runtime_error")
{
    TempDir tmp;
    const fs::path dir = tmp.path / "not-a-file.dsk";
    fs::create_directories(dir);
    CHECK_THROWS_AS(WorkingCopy(dir, tmp.path / "work"), std::runtime_error);
}
