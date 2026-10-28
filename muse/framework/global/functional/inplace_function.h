#pragma once

#include "global/thirdparty/sg14/inplace_function.h"

namespace muse::functional {
template<class Signature, size_t Capacity = stdext::inplace_function_detail::InplaceFunctionDefaultCapacity,
         size_t Alignment = alignof(stdext::inplace_function_detail::aligned_storage_t<Capacity>)>
using inplace_function = stdext::inplace_function<Signature, Capacity, Alignment>;
}
