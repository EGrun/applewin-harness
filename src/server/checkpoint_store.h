// SPDX-License-Identifier: MIT
#pragma once
// A session's checkpoints on disk (spec: docs/specs/m4.md, "API" checkpoint .. delete_checkpoint; D5). One directory per
// checkpoint, <workDir>/checkpoints/<id>/, holding AppleWin's state.yaml, a copy of every working disk, and meta.json
// (the harness metadata: name, created_at, SnapshotMeta, and which drive each disk copy belongs to).
// With a persistent directory (docs/specs/persistence.md 3.1, D18) the checkpoints live in <persistentDir>/<id>/ instead,
// survive the process, and are read back by the next store opened on that directory.
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

// Another process holds the persistent directory (its .lock is open without sharing).
struct CheckpointDirLocked : std::runtime_error {
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
        std::filesystem::path originalPath;  // the image the working copy came from (as list_disks reports it)
        std::string statePath;               // the disk's path exactly as the save state holds it ("" if not recorded)
    };

    // One disk to checkpoint (what the session knows about a mounted drive).
    struct DiskSource {
        int drive = 0;
        std::filesystem::path workingPath;
        std::filesystem::path originalPath;
        std::string statePath;
    };

    struct Entry {
        std::string id;          // "cp-<n>", n counting up from 1 and never reused
        std::string name;        // "" when none was given
        std::string createdAt;   // ISO 8601 UTC, e.g. "2026-10-17T12:34:56Z"
        SnapshotMeta meta;
        std::filesystem::path dir;   // <workDir>/checkpoints/<id>
        std::vector<DiskCopy> disks;
        std::string build;       // BuildId() of the harness that made it
        std::string runMode;     // "stepped" or "realtime", informational
        bool foreign = false;    // read from the directory at open, not made by this store
        std::filesystem::path StateFile() const { return dir / "state.yaml"; }
    };

    // workDir: the session's work directory (%TEMP%\applewin-harness\<pid>). Nothing is created until Create.
    explicit CheckpointStore(std::filesystem::path workDir);
    // Persistent: creates persistentDir if missing, takes <persistentDir>/.lock (held until the store is destroyed),
    // deletes leftover *.tmp directories and pending.yaml, and reads every complete cp-<n>/ (the others are left alone and
    // named by Skipped()). New ids continue past the highest cp-<n> directory, complete or not.
    // Throws CheckpointDirLocked if another process holds the directory, else std::runtime_error naming the cause.
    CheckpointStore(std::filesystem::path workDir, std::filesystem::path persistentDir);
    ~CheckpointStore();
    CheckpointStore(const CheckpointStore&) = delete;
    CheckpointStore& operator=(const CheckpointStore&) = delete;

    // Where Emulator::SaveSnapshot writes the next checkpoint's state before Create: <workDir>/checkpoints/pending.yaml,
    // or <persistentDir>/pending.yaml.
    std::filesystem::path PendingStateFile() const;
    // One message per cp-<n> directory skipped at open (unreadable meta.json, a missing file): "cp-<n>: <cause>".
    const std::vector<std::string>& Skipped() const { return skipped_; }

    // Makes the next entry: builds <id>.tmp/ (PendingStateFile() moved in as state.yaml, a copy of every disk in disks,
    // meta.json), then renames it to <id>/. runMode is recorded as given, build is BuildId(). Throws std::runtime_error naming the cause (the
    // session's -32004) if there are already kMaxCheckpoints entries or anything fails; a failed Create leaves no
    // directory behind and does not use up an id. name is not validated here (the session checks it).
    Entry Create(std::string name, const SnapshotMeta& meta, const std::vector<DiskSource>& disks, std::string runMode);
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
    std::filesystem::path Root() const;   // where the cp-<n> directories are
    void Open();                          // the persistent constructor's work, once the lock is taken
    Entry ReadEntry(const std::filesystem::path& dir, const std::string& id) const;

    std::filesystem::path workDir_;
    std::filesystem::path persistentDir_;   // empty: ephemeral
    void* lock_ = nullptr;                  // the .lock file's HANDLE
    std::vector<std::string> skipped_;
    uint64_t nextNumber_ = 1;
    std::map<uint64_t, Entry> entries_;   // by n, so iteration is oldest first
};

}  // namespace harness
