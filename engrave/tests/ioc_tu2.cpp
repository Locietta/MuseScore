#include "ioc_tu2.h"

namespace ioc_test {
const kors::modularity::ContextPtr& getTu2GlobalCtx() {
    return kors::modularity::globalCtx;
}

const kors::modularity::ContextPtr& getTu2GlobalContextInstance() {
    return kors::modularity::globalContextInstance();
}
}
