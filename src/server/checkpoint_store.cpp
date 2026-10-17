#include "server/checkpoint_store.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <fstream>
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

[[noreturn]] void Fail(const std::string& what, const std::error_code& ec)
{
    throw std::runtime_error(what + (ec ? ": " + ec.message() : std::string()));
}

}  // namespace

CheckpointStore::CheckpointStore(fs::path workDir) : workDir_(std::move(workDir)) {}

fs::path CheckpointStore::PendingStateFile() const
{
    std::error_code ec;
    fs::create_directories(workDir_ / "checkpoints", ec);   // a failure shows when the file is written
    return workDir_ / "checkpoints" / "pending.yaml";
}

CheckpointStore::Entry CheckpointStore::Create(std::string name, const SnapshotMeta& meta,
                                               const std::vector<DiskInfo>& disks)
{
    if (entries_.size() >= kMaxCheckpoints)
        throw std::runtime_error("checkpoint limit reached (" + std::to_string(kMaxCheckpoints) + ")");

    Entry e;
    e.id = "cp-" + std::to_string(nextNumber_);
    e.name = std::move(name);
    e.createdAt = NowIsoUtc();
    e.meta = meta;
    e.dir = workDir_ / "checkpoints" / e.id;

    std::error_code ec;
    try {
        fs::create_directories(e.dir, ec);
        if (ec) Fail("cannot create " + Utf8(e.dir), ec);
        fs::rename(PendingStateFile(), e.StateFile(), ec);
        if (ec) Fail("cannot move the save state into " + Utf8(e.dir), ec);
        for (const DiskInfo& d : disks) {
            DiskCopy c;
            c.drive = d.drive;
            c.workingPath = d.workingPath;
            c.copy = e.dir / ("disk" + std::to_string(d.drive + 1) + "-" + Utf8(d.workingPath.filename()));
            fs::copy_file(d.workingPath, c.copy, fs::copy_options::overwrite_existing, ec);
            if (ec) Fail("cannot copy disk image " + Utf8(d.workingPath), ec);
            e.disks.push_back(std::move(c));
        }
        json j;
        j["checkpoint_id"] = e.id;
        j["name"] = e.name;
        j["created_at"] = e.createdAt;
        j["emulated_cycles"] = meta.emulatedCycles;
        j["power_on_origin"] = meta.powerOnOrigin;
        j["speed"] = {{"machine_speed", meta.speed.machineSpeed},
                      {"enhance_disk", meta.speed.enhanceDisk},
                      {"full_speed_override", meta.speed.fullSpeedOverride}};
        j["joystick_x"] = meta.joystickX;
        j["joystick_y"] = meta.joystickY;
        json arr = json::array();
        for (const DiskCopy& c : e.disks)
            arr.push_back({{"drive", c.drive}, {"working_path", Utf8(c.workingPath)}, {"copy", Utf8(c.copy.filename())}});
        j["disks"] = arr;
        std::ofstream out(e.dir / "meta.json", std::ios::binary | std::ios::trunc);
        out << j.dump(2);
        out.close();
        if (!out) throw std::runtime_error("cannot write " + Utf8(e.dir / "meta.json"));
    } catch (...) {
        fs::remove_all(e.dir, ec);
        fs::remove(PendingStateFile(), ec);
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
        fs::remove_all(it->second.dir, ec);
        entries_.erase(it);
        return;
    }
    throw std::out_of_range("unknown checkpoint: " + id);
}

void CheckpointStore::RestoreDisks(const Entry& entry, const std::vector<DiskInfo>&)
{
    std::error_code ec;
    for (const DiskCopy& c : entry.disks)   // all present before anything is overwritten
        if (!fs::is_regular_file(c.copy, ec))
            throw std::runtime_error("checkpoint " + entry.id + ": disk copy missing: " + Utf8(c.copy));
    for (const DiskCopy& c : entry.disks) {
        fs::copy_file(c.copy, c.workingPath, fs::copy_options::overwrite_existing, ec);
        if (ec) Fail("cannot restore disk image " + Utf8(c.workingPath), ec);
    }
}

}  // namespace harness
