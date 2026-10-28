#pragma once
#ifdef __APPLE__
#include <os/signpost.h>

#include "global/defer.h"
#include "muse_framework_config.h"

#define MSS_SIGNPOST_CONCAT_IMPL(x, y) x##y
#define MSS_SIGNPOST_CONCAT(x, y) MSS_SIGNPOST_CONCAT_IMPL(x, y)

// Call this macro in your source file (at function or namespace scope) to initialize the signpost log.
#define MSS_SIGNPOST_PREPARE \
    static os_log_t sn_log = os_log_create(MUSE_APP_NAME_MACHINE_READABLE, OS_LOG_CATEGORY_POINTS_OF_INTEREST);

// Call these macros to begin and end a signpost interval
#define MSS_SIGNPOST_BEGIN(name)                \
    auto spid = os_signpost_id_generate(sn_log); \
    os_signpost_interval_begin(sn_log, spid, name);

#define MSS_SIGNPOST_END(name) os_signpost_interval_end(sn_log, spid, name);
#define MSS_SIGNPOST_SCOPE(name) MSS_SIGNPOST_SCOPE_IMPL(name, __COUNTER__)
#define MSS_SIGNPOST_SCOPE_IMPL(name, id)                                       \
    auto MSS_SIGNPOST_CONCAT(spid_, id) = os_signpost_id_generate(sn_log);      \
    os_signpost_interval_begin(sn_log, MSS_SIGNPOST_CONCAT(spid_, id), name);   \
    muse::Defer MSS_SIGNPOST_CONCAT(g_, id)([spid = MSS_SIGNPOST_CONCAT(spid_, id)] { \
        os_signpost_interval_end(sn_log, spid, name);                           \
    });

#define MSS_SIGNPOST_FUNCTION MSS_SIGNPOST_FUNCTION_IMPL(__COUNTER__)
#define MSS_SIGNPOST_FUNCTION_IMPL(id)                                                        \
    auto MSS_SIGNPOST_CONCAT(spid_, id) = os_signpost_id_generate(sn_log);                     \
    const char* MSS_SIGNPOST_CONCAT(function_, id) = __PRETTY_FUNCTION__;                      \
    os_signpost_interval_begin(sn_log, MSS_SIGNPOST_CONCAT(spid_, id), "Function", "%s",      \
                               MSS_SIGNPOST_CONCAT(function_, id));                            \
    muse::Defer MSS_SIGNPOST_CONCAT(g_, id)([spid = MSS_SIGNPOST_CONCAT(spid_, id),            \
                                             function = MSS_SIGNPOST_CONCAT(function_, id)] {   \
        os_signpost_interval_end(sn_log, spid, "Function", "%s", function);                   \
    });

#else  // !__APPLE__
#define MSS_SIGNPOST_PREPARE
#define MSS_SIGNPOST_BEGIN(name)
#define MSS_SIGNPOST_END(name)
#define MSS_SIGNPOST_SCOPE(name)
#define MSS_SIGNPOST_FUNCTION
#endif
