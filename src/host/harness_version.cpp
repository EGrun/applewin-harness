// SPDX-License-Identifier: MIT
#include "host/harness_version.h"

#ifndef HARNESS_VERSION
#error "HARNESS_VERSION is defined by build/Harness.props from the root VERSION file"
#endif

#ifndef APPLEWIN_COMMIT
#error "APPLEWIN_COMMIT is defined by build/Harness.props from the AppleWin submodule's commit"
#endif

namespace harness {

const char* Version()
{
    return HARNESS_VERSION;
}

const char* BuildId()
{
    return HARNESS_VERSION "+" APPLEWIN_COMMIT;
}

}  // namespace harness
