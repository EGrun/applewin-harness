#include "server/checkpoint_store.h"
#include <stdexcept>
#include <utility>

namespace harness {

// Stubs until M4 Task 3: every operation fails with std::runtime_error (Get and Delete: std::out_of_range).

CheckpointStore::CheckpointStore(std::filesystem::path workDir) : workDir_(std::move(workDir)) {}

std::filesystem::path CheckpointStore::PendingStateFile() const
{
    return workDir_ / "checkpoints" / "pending.yaml";
}

CheckpointStore::Entry CheckpointStore::Create(std::string, const SnapshotMeta&, const std::vector<DiskInfo>&)
{
    throw std::runtime_error("CheckpointStore::Create: not implemented yet (M4 Task 3)");
}

const CheckpointStore::Entry& CheckpointStore::Get(const std::string& id) const
{
    throw std::out_of_range("unknown checkpoint_id: " + id);
}

std::vector<CheckpointStore::Entry> CheckpointStore::List() const
{
    return {};
}

void CheckpointStore::Delete(const std::string& id)
{
    throw std::out_of_range("unknown checkpoint_id: " + id);
}

void CheckpointStore::RestoreDisks(const Entry&, const std::vector<DiskInfo>&)
{
    throw std::runtime_error("CheckpointStore::RestoreDisks: not implemented yet (M4 Task 3)");
}

}  // namespace harness
