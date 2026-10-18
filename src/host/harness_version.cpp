// SPDX-License-Identifier: MIT
#include "host/harness_version.h"

#ifndef HARNESS_VERSION
#error "HARNESS_VERSION is defined by build/Harness.props from the root VERSION file"
#endif

namespace harness {

const char* Version()
{
    return HARNESS_VERSION;
}

}  // namespace harness
