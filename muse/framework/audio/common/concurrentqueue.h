#pragma once

#if defined(__MACH__)
#include <mach/machine/vm_types.h>

#pragma push_macro("integer_t")
#define integer_t ::integer_t
#endif

#include "../thirdparty/moodycamel/blockingconcurrentqueue.h"

#if defined(__MACH__)
#pragma pop_macro("integer_t")
#endif

namespace muse {
template<typename T,
         typename Traits = moodycamel::ConcurrentQueueDefaultTraits>
using BlockingConcurrentQueue = moodycamel::BlockingConcurrentQueue<T, Traits>;
}
