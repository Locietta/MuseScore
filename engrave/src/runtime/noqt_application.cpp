#include "noqt_application.h"
#include "version.h"

#include "engrave_version.h"

using namespace muse;

namespace music {
const char* engine_version() { return MUSE_APP_VERSION; }

String NoQtApplication::name() const
{
#ifdef MUSE_APP_NAME
    return String::fromAscii(MUSE_APP_NAME);
#else
    return String();
#endif
}

String NoQtApplication::title() const
{
#ifdef MUSE_APP_TITLE
#ifdef MUSE_APP_UNSTABLE
    return String::fromAscii(MUSE_APP_TITLE) + u" Development";
#else
    return String::fromAscii(MUSE_APP_TITLE);
#endif
#else
    return String();
#endif
}

bool NoQtApplication::unstable() const
{
#ifdef MUSE_APP_UNSTABLE
    return true;
#else
    return false;
#endif
}

Version NoQtApplication::version() const
{
#ifdef MUSE_APP_VERSION
    static const Version v(MUSE_APP_VERSION);
#else
    static const Version v("0.0.0");
#endif
    return v;
}

Version NoQtApplication::fullVersion() const
{
    // Initialized once (thread-safe static); export workers may call this concurrently.
    static const Version fv = [this] {
        Version result(version());
#ifdef MUSE_APP_VERSION_LABEL
        const String label = String::fromAscii(MUSE_APP_VERSION_LABEL);
        if (!label.isEmpty()) {
            result.setSuffix(label);
        }
#endif
        return result;
    }();
    return fv;
}

String NoQtApplication::build() const
{
#ifdef MUSE_APP_BUILD_NUMBER
    return String::fromAscii(MUSE_APP_BUILD_NUMBER);
#else
    return String();
#endif
}

String NoQtApplication::revision() const
{
#ifdef MUSE_APP_REVISION
    return String::fromAscii(MUSE_APP_REVISION);
#else
    return String();
#endif
}

IApplication::RunMode NoQtApplication::runMode() const
{
    return RunMode::ConsoleApp;
}

bool NoQtApplication::noGui() const
{
    return true;
}

void NoQtApplication::setup()
{
}

void NoQtApplication::finish()
{
}

void NoQtApplication::restart()
{
}

modularity::ContextPtr NoQtApplication::setupNewContext(const StringList& args)
{
    // The headless host runs one engine per process and registers every service
    // in the process-wide IoC (see initialize_native_runtime), so its only
    // context is the global one. Upstream's BaseApplication::setupNewContext()
    // creates a numbered per-window context instead; on such a context
    // modularity::ioc(ctx) is a fresh, empty ModulesContextIoC, and lookups that
    // go through it directly (RWRegister::writer(score->iocContext()), the score
    // renderer) would no longer see the exports we register globally. ioc() maps
    // the global context back to globalIoc(), which is exactly what this host
    // needs for its single engine instance.
    (void)args;
    return modularity::globalCtx();
}

void NoQtApplication::destroyContext(const modularity::ContextPtr& ctx)
{
    // Nothing to tear down: the global context and its IoC live for the whole
    // process, and the host refuses a second engine instance.
    (void)ctx;
}

size_t NoQtApplication::contextCount() const
{
    return 1;
}

std::vector<modularity::ContextPtr> NoQtApplication::contexts() const
{
    return { modularity::globalCtx() };
}

void NoQtApplication::processEvents()
{
}
}
