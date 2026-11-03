// SPDX-License-Identifier: MIT
#include "server/checkpoint_store.h"
#include "host/harness_version.h"
#include <nlohmann/json.hpp>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace harness {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string Utf8(const fs::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string NowIsoUtc()
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

fs::path FromUtf8(const std::string& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

// An antivirus scanner, the indexer or a cloud-sync client may hold a freshly written file for a moment: a rename or a
// delete that fails with access denied or a sharing violation is tried again, about 5 times 75 ms apart.
template <class Op>
void RetryWhileBusy(Op op, std::error_code& ec)
{
    for (int attempt = 0;; ++attempt) {
        op(ec);
        const bool busy = ec && ec.category() == std::system_category() &&
                          (ec.value() == ERROR_ACCESS_DENIED || ec.value() == ERROR_SHARING_VIOLATION);
        if (!busy || attempt >= 4) return;
        Sleep(75);
    }
}

[[noreturn]] void Fail(const std::string& what, const std::error_code& ec)
{
    throw std::runtime_error(what + (ec ? ": " + ec.message() : std::string()));
}

}  // namespace

CheckpointStore::CheckpointStore(fs::path workDir) : workDir_(std::move(workDir)) {}

CheckpointStore::CheckpointStore(fs::path workDir, fs::path persistentDir)
    : workDir_(std::move(workDir)), persistentDir_(std::move(persistentDir))
{
    std::error_code ec;
    fs::create_directories(persistentDir_, ec);
    if (ec) Fail("cannot create " + Utf8(persistentDir_), ec);
    const fs::path lockFile = persistentDir_ / ".lock";
    HANDLE h = CreateFileW(lockFile.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
            throw CheckpointDirLocked("checkpoint directory is in use by another process: " + Utf8(persistentDir_));
        Fail("cannot open " + Utf8(lockFile), std::error_code(static_cast<int>(err), std::system_category()));
    }
    lock_ = h;
    try {
        Open();
    } catch (...) {
        CloseHandle(h);   // the destructor does not run for a constructor that throws
        throw;
    }
}

CheckpointStore::~CheckpointStore()
{
    if (lock_) CloseHandle(static_cast<HANDLE>(lock_));
}

fs::path CheckpointStore::Root() const
{
    return persistentDir_.empty() ? workDir_ / "checkpoints" : persistentDir_;
}

void CheckpointStore::Open()
{
    std::error_code ec;
    fs::remove(persistentDir_ / "pending.yaml", ec);
    static const std::regex kCp(R"(cp-(\d+))");
    uint64_t highest = 0;
    std::vector<std::pair<uint64_t, fs::path>> found;
    for (fs::directory_iterator it(persistentDir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code tec;
        if (!it->is_directory(tec)) continue;
        const std::string leaf = Utf8(it->path().filename());
        static const std::regex kTmp(R"(cp-\d+\.tmp)");   // only what Create leaves behind, never an operator's folder
        if (std::regex_match(leaf, kTmp)) {
            fs::remove_all(it->path(), tec);
            if (tec) Fail("cannot delete the leftover " + Utf8(it->path()), tec);
            continue;
        }
        std::smatch m;
        if (!std::regex_match(leaf, m, kCp)) continue;
        uint64_t n = 0;
        try {
            n = std::stoull(m[1].str());
        } catch (const std::exception&) {
            skipped_.push_back(leaf + ": the number is out of range");
            continue;
        }
        highest = std::max(highest, n);
        found.emplace_back(n, it->path());
    }
    if (ec) Fail("cannot read " + Utf8(persistentDir_), ec);
    for (const auto& [n, dir] : found) {
        const std::string id = "cp-" + std::to_string(n);
        if (Utf8(dir.filename()) != id) {   // "cp-007": not a name Create would have made
            skipped_.push_back(Utf8(dir.filename()) + ": not a canonical checkpoint id");
            continue;
        }
        try {
            entries_.emplace(n, ReadEntry(dir, id));
        } catch (const std::exception& e) {
            skipped_.push_back(id + ": " + e.what());
        }
    }
    nextNumber_ = highest + 1;
}

CheckpointStore::Entry CheckpointStore::ReadEntry(const fs::path& dir, const std::string& id) const
{
    std::ifstream in(dir / "meta.json", std::ios::binary);
    if (!in) throw std::runtime_error("meta.json is missing");
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw std::runtime_error("meta.json does not parse");
    Entry e;
    try {
        if (j.at("checkpoint_id").get<std::string>() != id) throw std::runtime_error("meta.json names another checkpoint");
        e.id = id;
        e.name = j.at("name").get<std::string>();
        e.createdAt = j.at("created_at").get<std::string>();
        e.meta.emulatedCycles = j.at("emulated_cycles").get<uint64_t>();
        e.meta.powerOnOrigin = j.at("power_on_origin").get<uint64_t>();
        const json& sp = j.at("speed");
        e.meta.speed.machineSpeed = sp.at("machine_speed").get<uint32_t>();
        e.meta.speed.enhanceDisk = sp.at("enhance_disk").get<bool>();
        e.meta.speed.fullSpeedOverride = sp.at("full_speed_override").get<bool>();
        e.meta.joystickX = static_cast<uint8_t>(j.at("joystick_x").get<int>());
        e.meta.joystickY = static_cast<uint8_t>(j.at("joystick_y").get<int>());
        e.build = j.at("build").get<std::string>();
        e.runMode = j.at("run_mode").get<std::string>();
        e.dir = dir;
        e.foreign = true;
        for (const json& d : j.at("disks")) {
            DiskCopy c;
            c.drive = d.at("drive").get<int>();
            c.workingPath = FromUtf8(d.at("working_path").get<std::string>());
            c.copy = dir / FromUtf8(d.at("copy").get<std::string>()).filename();
            c.originalPath = FromUtf8(d.at("original_path").get<std::string>());
            c.statePath = d.at("state_path").get<std::string>();
            e.disks.push_back(std::move(c));
        }
    } catch (const json::exception& ex) {
        throw std::runtime_error(std::string("meta.json is malformed: ") + ex.what());
    }
    std::error_code ec;
    if (!fs::is_regular_file(e.StateFile(), ec)) throw std::runtime_error("state.yaml is missing");
    for (const DiskCopy& c : e.disks)
        if (!fs::is_regular_file(c.copy, ec)) throw std::runtime_error("disk copy missing: " + Utf8(c.copy.filename()));
    return e;
}

fs::path CheckpointStore::PendingStateFile() const
{
    std::error_code ec;
    fs::create_directories(Root(), ec);   // a failure shows when the file is written
    return Root() / "pending.yaml";
}

CheckpointStore::Entry CheckpointStore::Create(std::string name, const SnapshotMeta& meta,
                                               const std::vector<DiskSource>& disks, std::string runMode)
{
    if (entries_.size() >= kMaxCheckpoints)
        throw std::runtime_error("checkpoint limit reached (" + std::to_string(kMaxCheckpoints) + ")");

    Entry e;
    e.id = "cp-" + std::to_string(nextNumber_);
    e.name = std::move(name);
    e.createdAt = NowIsoUtc();
    e.meta = meta;
    e.build = BuildId();
    e.runMode = std::move(runMode);
    e.dir = Root() / e.id;
    const fs::path tmp = Root() / (e.id + ".tmp");   // renamed to e.dir once complete: a crash never leaves half a checkpoint

    std::error_code ec;
    try {
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        if (ec) Fail("cannot create " + Utf8(tmp), ec);
        fs::rename(PendingStateFile(), tmp / "state.yaml", ec);
        if (ec) Fail("cannot move the save state into " + Utf8(tmp), ec);
        for (const DiskSource& d : disks) {
            DiskCopy c;
            c.drive = d.drive;
            c.workingPath = d.workingPath;
            c.originalPath = d.originalPath;
            c.statePath = d.statePath;
            const std::string leaf = "disk" + std::to_string(d.drive + 1) + "-" + Utf8(d.workingPath.filename());
            c.copy = e.dir / leaf;
            fs::copy_file(d.workingPath, tmp / leaf, fs::copy_options::overwrite_existing, ec);
            if (ec) Fail("cannot copy disk image " + Utf8(d.workingPath), ec);
            e.disks.push_back(std::move(c));
        }
        json j;
        j["checkpoint_id"] = e.id;
        j["name"] = e.name;
        j["created_at"] = e.createdAt;
        j["build"] = e.build;
        j["run_mode"] = e.runMode;
        j["emulated_cycles"] = meta.emulatedCycles;
        j["power_on_origin"] = meta.powerOnOrigin;
        j["speed"] = {{"machine_speed", meta.speed.machineSpeed},
                      {"enhance_disk", meta.speed.enhanceDisk},
                      {"full_speed_override", meta.speed.fullSpeedOverride}};
        j["joystick_x"] = meta.joystickX;
        j["joystick_y"] = meta.joystickY;
        json arr = json::array();
        for (const DiskCopy& c : e.disks)
            arr.push_back({{"drive", c.drive},
                           {"working_path", Utf8(c.workingPath)},
                           {"copy", Utf8(c.copy.filename())},
                           {"original_path", Utf8(c.originalPath)},
                           {"state_path", c.statePath}});
        j["disks"] = arr;
        std::ofstream out(tmp / "meta.json", std::ios::binary | std::ios::trunc);
        out << j.dump(2);
        out.close();
        if (!out) throw std::runtime_error("cannot write " + Utf8(tmp / "meta.json"));
        // The .tmp name protects against the process dying mid-write, not against power loss: nothing is flushed.
        RetryWhileBusy([&](std::error_code& c) { fs::rename(tmp, e.dir, c); }, ec);
        if (ec) Fail("cannot move " + Utf8(tmp) + " to " + Utf8(e.dir), ec);
    } catch (...) {
        fs::remove_all(tmp, ec);
        throw;
    }
    entries_.emplace(nextNumber_, e);
    ++nextNumber_;
    return e;
}

const CheckpointStore::Entry& CheckpointStore::Get(const std::string& id) const
{
    for (const auto& [n, e] : entries_)
        if (e.id == id) return e;
    throw std::out_of_range("unknown checkpoint: " + id);
}

std::vector<CheckpointStore::Entry> CheckpointStore::List() const
{
    std::vector<Entry> out;
    out.reserve(entries_.size());
    for (const auto& [n, e] : entries_) out.push_back(e);
    return out;
}

void CheckpointStore::Delete(const std::string& id)
{
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.id != id) continue;
        std::error_code ec;
        RetryWhileBusy([&](std::error_code& c) { fs::remove_all(it->second.dir, c); }, ec);
        if (ec) throw std::runtime_error("cannot delete " + Utf8(it->second.dir) + ": " + ec.message());
        entries_.erase(it);
        return;
    }
    throw std::out_of_range("unknown checkpoint: " + id);
}

namespace {
fs::path BackupOf(const fs::path& p)
{
    return fs::path(p.native() + L".restore-bak");
}
}  // namespace

void CheckpointStore::PrepareRestore(const Entry& entry)
{
    std::error_code ec;
    for (const DiskCopy& c : entry.disks)   // the drives may hold other disks now: only the checkpoint's own files matter
        if (!fs::is_regular_file(c.copy, ec))
            throw std::runtime_error("checkpoint " + entry.id + ": disk copy missing: " + Utf8(c.copy));
    for (const DiskCopy& c : entry.disks) {   // a stale backup would be "rolled back" over a good working copy
        const fs::path bak = BackupOf(c.workingPath);
        fs::remove(bak, ec);
        if (fs::exists(fs::symlink_status(bak, ec)) || (ec && ec != std::errc::no_such_file_or_directory))
            throw std::runtime_error("checkpoint " + entry.id + ": cannot remove the stale " + Utf8(bak));
    }
}

void CheckpointStore::RestoreDisks(const Entry& entry)
{
    std::error_code ec;
    for (const DiskCopy& c : entry.disks) {   // all present, no backup in the way, before anything is touched
        if (!fs::is_regular_file(c.copy, ec))
            throw std::runtime_error("checkpoint " + entry.id + ": disk copy missing: " + Utf8(c.copy));
        if (fs::exists(fs::symlink_status(BackupOf(c.workingPath), ec)))
            throw std::runtime_error("checkpoint " + entry.id + ": a backup is in the way: " +
                                     Utf8(BackupOf(c.workingPath)));
    }
    try {
        for (const DiskCopy& c : entry.disks) {   // working copies aside
            fs::rename(c.workingPath, BackupOf(c.workingPath), ec);
            if (ec) Fail("cannot move " + Utf8(c.workingPath) + " aside", ec);
        }
        for (const DiskCopy& c : entry.disks) {
            fs::copy_file(c.copy, c.workingPath, fs::copy_options::overwrite_existing, ec);
            if (ec) Fail("cannot restore disk image " + Utf8(c.workingPath), ec);
        }
    } catch (const std::exception& e) {
        const std::string rollback = RollbackRestore(entry);
        if (!rollback.empty()) throw RestoreRollbackError(std::string(e.what()) + "; " + rollback);
        throw;
    }
}

void CheckpointStore::CommitRestore(const Entry& entry) noexcept
{
    std::error_code ec;
    for (const DiskCopy& c : entry.disks) fs::remove(BackupOf(c.workingPath), ec);
}

std::string CheckpointStore::RollbackRestore(const Entry& entry)
{
    std::string failed;
    std::error_code ec;
    for (const DiskCopy& c : entry.disks) {
        const fs::path bak = BackupOf(c.workingPath);
        if (!fs::exists(bak, ec)) continue;   // not moved aside (PrepareRestore removed any stale backup)
        fs::remove(c.workingPath, ec);
        fs::rename(bak, c.workingPath, ec);
        if (ec) failed += (failed.empty() ? "" : "; ") + ("cannot put back " + Utf8(c.workingPath) + ": " + ec.message());
    }
    return failed.empty() ? failed : "rolling back failed: " + failed;
}

}  // namespace harness
