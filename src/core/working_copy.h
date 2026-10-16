#pragma once
#include <filesystem>

namespace harness {

// The working copy that D5 requires. The original is only ever opened for reading.
class WorkingCopy {
public:
    // Creates workDir if needed and copies original to workDir / original.filename(), then clears the read-only
    // attribute on the copy. Throws std::runtime_error naming the file if original doesn't exist, isn't a regular file,
    // or can't be copied.
    WorkingCopy(const std::filesystem::path& original, const std::filesystem::path& workDir);
    // Deletes the copy, and also workDir if it is then empty. Never throws.
    ~WorkingCopy();

    WorkingCopy(WorkingCopy&& other) noexcept;
    WorkingCopy& operator=(WorkingCopy&& other) noexcept;
    WorkingCopy(const WorkingCopy&) = delete;
    WorkingCopy& operator=(const WorkingCopy&) = delete;

    const std::filesystem::path& Path() const;   // the copy

private:
    std::filesystem::path path_;
    std::filesystem::path workDir_;
};

}  // namespace harness
