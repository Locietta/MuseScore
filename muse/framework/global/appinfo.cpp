/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "appinfo.h"

#include "engrave_version.h"

namespace muse::appinfo {
const char* versionString()
{
    return MUSE_APP_VERSION;
}

Version version()
{
    static const Version v(MUSE_APP_VERSION);
    return v;
}

String revision()
{
#ifdef MUSE_APP_REVISION
    return String::fromAscii(MUSE_APP_REVISION);
#else
    return String();
#endif
}
}