#pragma once
#include "support/golden.h"
#include <filesystem>

namespace harness::testsupport {

// The repository root: <test exe dir>/../../.. (bin/x64/<Configuration>/HarnessTests.exe).
inline std::filesystem::path RepoRoot()
{
    return std::filesystem::weakly_canonical(TestExeDir() / ".." / ".." / "..");
}

// The DOS 3.3 System Master image in the AppleWin submodule. Tests must never open it for writing.
inline std::filesystem::path DosMasterImage()
{
    return RepoRoot() / "external" / "AppleWin" / "bin" / "DOS 3.3 System Master - 680-0210-A.dsk";
}

}  // namespace harness::testsupport
