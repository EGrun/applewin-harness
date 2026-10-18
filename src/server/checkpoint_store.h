#pragma once
// A session's checkpoints on disk (spec: docs/specs/m4.md, "API" checkpoint .. delete_checkpoint; D5). One directory per
// checkpoint, <workDir>/checkpoints/<id>/, holding AppleWin's state.yaml, a copy of every working disk, and meta.json
// (the harness metadata: name, created_at, SnapshotMeta, and which drive each disk copy belongs to).
#include "host/emulator.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace harness {

// RestoreDisks failed and so did putting the working copies back: they no longer match what the drives held.
struct RestoreRollbackError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

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
    // Review I-3: every disk check that needs no ejected drive, run BEFORE Emulator::EjectAll. The drives may hold other
    // disks than the checkpoint did (insert_disk, eject_disk): the checkpoint's working copies stay on disk until
    // close_session, so every disk copy must exist, and a stale <working copy>.restore-bak left by an earlier crash is
    // deleted (so a rollback can never put it over a good disk).
    // Throws std::runtime_error naming the cause (the session's -32004); nothing is changed but stale backups.
    void PrepareRestore(const Entry& entry);
    // Copies each of the entry's disk copies over its working copy (the drives are now ejected by Emulator::EjectAll).
    // Throws std::runtime_error naming the file if a copy is missing, a backup is in the way, or a copy can't be written
    // (the session's -32004).
    // Each working copy is first renamed to <name>.restore-bak. If anything fails the partial copies are deleted, the
    // backups renamed back (nothing is lost) and std::runtime_error thrown; if putting a backup back fails too,
    // RestoreRollbackError (the working copies are then not what the drives held). On success the backups are kept
    // until CommitRestore (the load worked) or RollbackRestore (it failed before touching the machine).
    void RestoreDisks(const Entry& entry);
    void CommitRestore(const Entry& entry) noexcept;   // deletes the backups
    // Restores the working copies from the backups. Returns "" when every backup was put back, else what failed.
    [[nodiscard]] std::string RollbackRestore(const Entry& entry);

private:
    std::filesystem::path workDir_;
    uint64_t nextNumber_ = 1;
    std::map<uint64_t, Entry> entries_;   // by n, so iteration is oldest first
};

}  // namespace harness
