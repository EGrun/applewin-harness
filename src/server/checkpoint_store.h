#pragma once
// A session's checkpoints on disk (spec: docs/specs/m4.md, "API" checkpoint .. delete_checkpoint; D5). One directory per
// checkpoint, <workDir>/checkpoints/<id>/, holding AppleWin's state.yaml, a copy of every working disk, and meta.json
// (the harness metadata: name, created_at, SnapshotMeta, and which drive each disk copy belongs to).
#include "host/emulator.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace harness {

class CheckpointStore {
public:
    static constexpr size_t kMaxCheckpoints = 256;
    static constexpr size_t kMaxNameChars = 64;

    struct DiskCopy {
        int drive = 0;                       // 0 or 1, as DiskInfo::drive
        std::filesystem::path workingPath;   // the working copy it was taken from (where RestoreDisks puts it back)
        std::filesystem::path copy;          // the file inside the checkpoint's directory
    };

    struct Entry {
        std::string id;          // "cp-<n>", n counting up from 1 and never reused
        std::string name;        // "" when none was given
        std::string createdAt;   // ISO 8601 UTC, e.g. "2026-10-17T12:34:56Z"
        SnapshotMeta meta;
        std::filesystem::path dir;   // <workDir>/checkpoints/<id>
        std::vector<DiskCopy> disks;
        std::filesystem::path StateFile() const { return dir / "state.yaml"; }
    };

    // workDir: the session's work directory (%TEMP%\applewin-harness\<pid>). Nothing is created until Create.
    explicit CheckpointStore(std::filesystem::path workDir);

    // Where Emulator::SaveSnapshot writes the next checkpoint's state before Create: <workDir>/checkpoints/pending.yaml.
    std::filesystem::path PendingStateFile() const;

    // Makes the next entry: creates its directory, moves PendingStateFile() in as state.yaml, copies every disk in
    // disks (DiskInfo::workingPath) into it, and writes meta.json. Throws std::runtime_error naming the cause (the
    // session's -32004) if there are already kMaxCheckpoints entries or anything fails; a failed Create leaves no
    // directory behind and does not use up an id. name is not validated here (the session checks it).
    Entry Create(std::string name, const SnapshotMeta& meta, const std::vector<DiskInfo>& disks);
    // std::out_of_range for an unknown id (the session's -32602).
    const Entry& Get(const std::string& id) const;
    // Every entry, oldest first.
    std::vector<Entry> List() const;
    // Removes the entry and its directory. std::out_of_range for an unknown id.
    void Delete(const std::string& id);
    // Copies each of the entry's disk copies over its working copy (current: the drives as they are now, already
    // ejected by Emulator::EjectAll). Throws std::runtime_error naming the file if a copy is missing or can't be
    // written (the session's -32004).
    void RestoreDisks(const Entry& entry, const std::vector<DiskInfo>& current);

private:
    std::filesystem::path workDir_;
    uint64_t nextNumber_ = 1;
    std::map<uint64_t, Entry> entries_;   // by n, so iteration is oldest first
};

}  // namespace harness
