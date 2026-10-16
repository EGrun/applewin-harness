#include "core/working_copy.h"

#include <stdexcept>
#include <system_error>
#include <utility>

namespace harness {

namespace fs = std::filesystem;

WorkingCopy::WorkingCopy(const fs::path& original, const fs::path& workDir)
{
    std::error_code ec;
    if (!fs::is_regular_file(original, ec))
        throw std::runtime_error("WorkingCopy: not a regular file: " + original.string());
    fs::create_directories(workDir, ec);
    if (ec)
        throw std::runtime_error("WorkingCopy: cannot create " + workDir.string() + ": " + ec.message());
    const fs::path dest = workDir / original.filename();
    if (!fs::copy_file(original, dest, fs::copy_options::overwrite_existing, ec) || ec)
        throw std::runtime_error("WorkingCopy: cannot copy " + original.string() + " to " + dest.string() + ": " + ec.message());
    path_ = dest;
    workDir_ = workDir;
    fs::permissions(path_, fs::perms::owner_write, fs::perm_options::add, ec);
    if (ec) {
        const std::string msg = ec.message();
        fs::remove(path_, ec);
        path_.clear();
        throw std::runtime_error("WorkingCopy: cannot clear read-only on " + dest.string() + ": " + msg);
    }
}

WorkingCopy::~WorkingCopy()
{
    if (path_.empty())
        return;
    std::error_code ec;
    fs::remove(path_, ec);
    if (!workDir_.empty() && fs::is_empty(workDir_, ec) && !ec)
        fs::remove(workDir_, ec);
}

WorkingCopy::WorkingCopy(WorkingCopy&& other) noexcept
    : path_(std::exchange(other.path_, {})), workDir_(std::exchange(other.workDir_, {}))
{
}

WorkingCopy& WorkingCopy::operator=(WorkingCopy&& other) noexcept
{
    if (this != &other) {
        this->~WorkingCopy();
        path_ = std::exchange(other.path_, {});
        workDir_ = std::exchange(other.workDir_, {});
    }
    return *this;
}

const fs::path& WorkingCopy::Path() const { return path_; }

}  // namespace harness
