// SPDX-License-Identifier: MIT
#pragma once

namespace harness {

// The harness version: the contents of the root VERSION file, read by the build (build/Harness.props).
const char* Version();

// The build a checkpoint is tied to: Version() + "+" + the first 12 hex digits of the AppleWin submodule commit, or
// "unknown" when the build could not run git (D18). A save state only loads in the build that made it.
const char* BuildId();

}  // namespace harness
