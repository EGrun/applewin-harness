#include "core/working_copy.h"

#include <stdexcept>
#include <utility>

namespace harness {

// Task 2 stubs: the interface is final, the behaviour arrives in a later task.

WorkingCopy::WorkingCopy(const std::filesystem::path&, const std::filesystem::path&)
{
    throw std::logic_error("not implemented");
}

WorkingCopy::~WorkingCopy() = default;

WorkingCopy::WorkingCopy(WorkingCopy&& other) noexcept
    : path_(std::exchange(other.path_, {})), workDir_(std::exchange(other.workDir_, {}))
{
}

WorkingCopy& WorkingCopy::operator=(WorkingCopy&& other) noexcept
{
    if (this != &other) {
        path_ = std::exchange(other.path_, {});
        workDir_ = std::exchange(other.workDir_, {});
    }
    return *this;
}

const std::filesystem::path& WorkingCopy::Path() const { return path_; }

}  // namespace harness
