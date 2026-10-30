#include "engine_audio.h"
#include "noqt_filesystem.h"
#include "settings.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static void expect(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}

static std::string utf8String(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}



#include "global/modularity/ioc.h"
#include "native/tests/ioc_tu2.h"

#ifdef MODULENAME
#undef MODULENAME
#endif
#define MODULENAME "ioc_contract_test"

namespace {

class IRegisteredGlobalService : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IRegisteredGlobalService)
public:
    virtual ~IRegisteredGlobalService() = default;
    virtual int globalValue() const = 0;
};

class RegisteredGlobalServiceImpl : public IRegisteredGlobalService
{
public:
    int globalValue() const override { return 42; }
};

class IRegisteredContextService : MODULE_CONTEXT_INTERFACE
{
    INTERFACE_ID(IRegisteredContextService)
public:
    virtual ~IRegisteredContextService() = default;
    virtual int contextValue() const = 0;
};

class RegisteredContextServiceImpl : public IRegisteredContextService
{
public:
    explicit RegisteredContextServiceImpl(int value) : m_value(value) {}
    int contextValue() const override { return m_value; }

private:
    int m_value = 0;
};

class IUnregisteredGlobalService : MODULE_GLOBAL_INTERFACE
{
    INTERFACE_ID(IUnregisteredGlobalService)
public:
    virtual ~IUnregisteredGlobalService() = default;
    virtual int dummy() const = 0;
};

class IUnregisteredContextService : MODULE_CONTEXT_INTERFACE
{
    INTERFACE_ID(IUnregisteredContextService)
public:
    virtual ~IUnregisteredContextService() = default;
    virtual int dummy() const = 0;
};

/*
 * =========================================================================
 * Kors Modularity (Qt-free IoC) Contract Documentation
 * =========================================================================
 * Upstream kors_modularity was adapted for NO_QT_SUPPORT headless operation
 * via commits f88394cf5d and ef8805ea94. This test pins the following contract:
 *
 * 1. globalCtx Singleton & Multi-TU Consistency (f88394cf5d):
 *    Under NO_QT_SUPPORT, globalContextInstance() is an inline function returning
 *    a function-local static std::shared_ptr<Context>(globalId = 0).
 *    By ISO C++ [dcl.inline], function-local statics in inline functions are
 *    guaranteed to refer to the exact same object across all translation units.
 *    globalCtx is a reference to this singleton. It is non-null, has ID 0, and
 *    is identical (both by pointer value and reference identity) across all TUs.
 *
 * 2. Context ID 0 Registry and globalIoc Mapping (ef8805ea94):
 *    Under NO_QT_SUPPORT typed IoC (IOC_CHECK_INTERFACE_TYPE), context ID 0
 *    is permitted: ioc(ctx) with ctx->id == 0 (including globalCtx and any
 *    user-created Context(0)) returns the shared context-0 ModulesContextIoC
 *    registry. Any context with id == 0 resolves the same service instance.
 *    removeIoC() refuses to delete context 0 (guard: ctx->id > 0).
 *
 * 3. Safe Null Resolution on Missing Registrations (ef8805ea94):
 *    Under NO_QT_SUPPORT, InjectBase::doResolve guards against a null IoC
 *    pointer and returns silently. If an interface is not registered,
 *    ioc->resolve<I>() returns nullptr and InjectBase::m_i remains nullptr
 *    without asserting or crashing.
 *    Caller-visible behavior: Callers must check if the injection resolved
 *    (e.g., if (!inject()) { ... }) and detect missing dependencies to log
 *    errors or fail clearly for required services.
 *
 * 4. Registered Exports Resolve In Their Own Registry:
 *    Global exports registered via globalIoc()->registerExport<I>(module, impl)
 *    resolve via GlobalInject. Contextual exports registered via
 *    ioc(ctx)->registerExport<I>(module, impl) resolve via ContextInject, and
 *    only for that context: registries are keyed by context id, so distinct
 *    ids do not see each other's exports and never see the global registry.
 *
 * 5. Context Removal:
 *    removeIoC(ctx) refuses context 0 and a null context. For a non-zero id it
 *    deletes that registry: the exports registered there become unresolvable,
 *    an injection that already resolved from it reports null (the registry
 *    notifies its subscribers while it is destroyed), and every other context
 *    as well as the global registry keep their services. The id can be reused
 *    and the replacement registry starts empty.
 *
 * 6. Reset:
 *    resetAll() clears the services that are still registered - the global
 *    registry plus every live context registry - without any unregister() call.
 *    The registries release their references (the caller's shared_ptr becomes
 *    the only owner) and already-resolved injections report null.
 *
 * 7. Required Dependencies:
 *    A required export that is missing resolves to null; resolve() and the
 *    injections never throw or assert. The host turns that null into a clear
 *    failure at its own call sites:
 *      native/engine_audio.cpp:464-466   IFileSystem    -> "native runtime (IFileSystem) is not initialised; ..."
 *      native/score_render.cpp:38-39     IScoreRenderer -> "Score renderer unavailable"
 *      native/cairo_painter.cpp:390-392  IFontsDatabase -> "CairoPainter: IFontsDatabase is not registered"
 *    Those call sites live in translation units this test binary does not link,
 *    except the first: engine_audio is linked here, and section 8 executes it.
 *    What the other two guards rely on is pinned as a condition: after
 *    resetAll() a required lookup is null and an injection that resolved
 *    earlier reports null as well, so a host guard cannot keep using a cleared
 *    required service.
 *
 * 8. Host Failure Without the File-System Service:
 *    initialize_engine_audio() (native/engine_audio.cpp:438) is a real host
 *    entry point that requires IFileSystem - it turns the SoundFont path into
 *    an absolute Uri. With no such export registered it must return false, fill
 *    `error` with its documented message, leave no engine behind, and not
 *    crash; the next engine entry point must report the same not-initialised
 *    state instead of half-working. The successful path is not driven here: it
 *    needs a real SoundFont and leaves process-wide engine state that cannot be
 *    re-initialised, so only the guard and the state it leaves are exercised.
 * =========================================================================
 */
void test_ioc_contract(const std::filesystem::path& assetsRoot)
{
    using namespace kors::modularity;

    // 1. globalCtx is non-null and identical across TUs
    expect(globalCtx != nullptr, "globalCtx is non-null");
    expect(globalCtx->id == 0, "globalCtx id is 0 (globalId)");
    expect(globalContextInstance() != nullptr, "globalContextInstance() is non-null");
    expect(&globalCtx == &globalContextInstance(), "globalCtx references globalContextInstance()");

    // Cross-TU verification against ioc_tu2.cpp
    const ContextPtr& tu2Ctx = ioc_test::getTu2GlobalCtx();
    const ContextPtr& tu2Inst = ioc_test::getTu2GlobalContextInstance();
    expect(tu2Ctx != nullptr, "TU2 globalCtx is non-null");
    expect(tu2Ctx->id == 0, "TU2 globalCtx id is 0");
    expect(globalCtx.get() == tu2Ctx.get(), "globalCtx instance pointer identical across TUs");
    expect(&globalCtx == &tu2Ctx, "globalCtx reference address identical across TUs");
    expect(&globalContextInstance() == &tu2Inst, "globalContextInstance() address identical across TUs");

    // 2. Context ID 0 and ioc mapping
    ModulesGlobalIoC* gIoc = globalIoc();
    expect(gIoc != nullptr, "globalIoc() is non-null");

    ModulesContextIoC* c0Ioc = ioc(globalCtx);
    expect(c0Ioc != nullptr, "ioc(globalCtx) is non-null");

    auto customCtx0 = std::make_shared<Context>(0);
    expect(ioc(customCtx0) == c0Ioc, "ioc(custom context with id 0) returns the same context-0 instance");

    // Null context returns nullptr under NO_QT_SUPPORT
    expect(ioc(ContextPtr{}) == nullptr, "ioc(null ContextPtr) returns nullptr under NO_QT_SUPPORT");

    // removeIoC refuses to remove context 0 or null
    removeIoC(globalCtx);
    expect(ioc(globalCtx) == c0Ioc, "removeIoC(globalCtx) refuses id 0");
    removeIoC(customCtx0);
    expect(ioc(customCtx0) == c0Ioc, "removeIoC(customCtx0) refuses id 0");
    removeIoC(ContextPtr{});
    expect(ioc(globalCtx) == c0Ioc, "removeIoC(null) safely ignored");

    // Context isolation for non-zero IDs
    auto ctx1 = std::make_shared<Context>(1);
    auto ctx2 = std::make_shared<Context>(2);
    ModulesContextIoC* ioc1 = ioc(ctx1);
    ModulesContextIoC* ioc2 = ioc(ctx2);
    expect(ioc1 != nullptr && ioc2 != nullptr, "non-zero context IoCs are non-null");
    expect(ioc1 != ioc2, "context 1 and context 2 are isolated instances");
    expect(ioc1 != c0Ioc && ioc2 != c0Ioc, "non-zero contexts are isolated from context 0");

    // 3. Registered exports resolve, but only inside their own registry
    // 3a. Global export
    auto globalImpl = std::make_shared<RegisteredGlobalServiceImpl>();
    gIoc->registerExport<IRegisteredGlobalService>("contract_test", globalImpl);

    GlobalInject<IRegisteredGlobalService> globalInj;
    expect(globalInj() == globalImpl, "GlobalInject resolves registered export");
    expect(globalInj()->globalValue() == 42, "Global export is functional");
    expect(gIoc->resolve<IRegisteredGlobalService>("contract_test") == globalImpl,
           "globalIoc()->resolve returns registered export");

    // 3b. One contextual export per registry: context 0, context 1, context 2
    auto contextImpl = std::make_shared<RegisteredContextServiceImpl>(100);
    auto ctx1Impl = std::make_shared<RegisteredContextServiceImpl>(7);
    auto ctx2Impl = std::make_shared<RegisteredContextServiceImpl>(8);
    c0Ioc->registerExport<IRegisteredContextService>("contract_test", contextImpl);
    ioc1->registerExport<IRegisteredContextService>("contract_test", ctx1Impl);
    ioc2->registerExport<IRegisteredContextService>("contract_test", ctx2Impl);

    expect(c0Ioc->resolve<IRegisteredContextService>("contract_test") == contextImpl,
           "context 0 resolves its own export");
    expect(ioc1->resolve<IRegisteredContextService>("contract_test") == ctx1Impl,
           "context 1 resolves its own export");
    expect(ioc2->resolve<IRegisteredContextService>("contract_test") == ctx2Impl,
           "context 2 resolves its own export");
    expect(contextImpl != ctx1Impl && contextImpl != ctx2Impl && ctx1Impl != ctx2Impl,
           "the three contexts hold distinct instances");

    ContextInject<IRegisteredContextService> ctx0Inj(globalCtx);
    ContextInject<IRegisteredContextService> ctx1Inj(ctx1);
    ContextInject<IRegisteredContextService> ctx2Inj(ctx2);
    expect(ctx0Inj() == contextImpl, "ContextInject(globalCtx) resolves the context-0 export");
    expect(ctx0Inj()->contextValue() == 100, "context-0 export is functional");
    expect(ctx1Inj() == ctx1Impl, "ContextInject(ctx1) resolves the context-1 export");
    expect(ctx1Inj()->contextValue() == 7, "context-1 export is functional");
    expect(ctx2Inj() == ctx2Impl, "ContextInject(ctx2) resolves the context-2 export");
    expect(ctx2Inj()->contextValue() == 8, "context-2 export is functional");

    ContextInject<IRegisteredContextService> ctx0AliasInj(customCtx0);
    expect(ctx0AliasInj() == contextImpl, "ContextInject(customCtx0) resolves same instance as globalCtx");

    // 4. An injection whose interface is NOT registered yields null
    //    Caller-visible behavior: doResolve returns silently without throwing or asserting,
    //    leaving m_i == nullptr. The caller must detect missing registrations and handle/log.
    GlobalInject<IUnregisteredGlobalService> unregGlobal;
    expect(unregGlobal() == nullptr, "unregistered GlobalInject yields nullptr");
    expect(unregGlobal.get() == nullptr, "unregistered GlobalInject get() is nullptr");

    ContextInject<IUnregisteredContextService> unregCtx(globalCtx);
    expect(unregCtx() == nullptr, "unregistered ContextInject(globalCtx) yields nullptr");

    // Null context injection: doResolve guards if (!ioc) return; without crashing
    ContextInject<IUnregisteredContextService> unregNull(ContextPtr{});
    expect(unregNull() == nullptr, "unregistered ContextInject(nullptr) yields nullptr safely");

    // 5. removeIoC drops one non-zero registry and leaves the others registered
    removeIoC(ctx1);
    ModulesContextIoC* ioc1Again = ioc(ctx1);
    expect(ioc1Again != nullptr, "a removed context id can be used again");
    expect(ioc1Again->resolve<IRegisteredContextService>("contract_test") == nullptr,
           "removeIoC makes the context-1 export unresolvable");
    expect(ctx1Inj() == nullptr, "an already-resolved context-1 injection reports null after removeIoC");

    expect(ioc(ctx2)->resolve<IRegisteredContextService>("contract_test") == ctx2Impl,
           "removeIoC(ctx1) leaves the context-2 export registered");
    expect(ctx2Inj() == ctx2Impl, "a context-2 injection still resolves after removeIoC(ctx1)");
    expect(ioc(globalCtx)->resolve<IRegisteredContextService>("contract_test") == contextImpl,
           "removeIoC(ctx1) leaves the context-0 export registered");
    expect(gIoc->resolve<IRegisteredGlobalService>("contract_test") == globalImpl,
           "removeIoC(ctx1) leaves the global export registered");

    // 6. resetAll clears services that are still registered: nothing below is
    //    unregistered first, so a no-op reset cannot pass these assertions.
    resetAll();
    expect(gIoc->resolve<IRegisteredGlobalService>("contract_test") == nullptr,
           "resetAll clears a registered global export");
    expect(globalInj() == nullptr, "an already-resolved GlobalInject reports null after resetAll");
    expect(c0Ioc->resolve<IRegisteredContextService>("contract_test") == nullptr,
           "resetAll clears a registered context-0 export");
    expect(ioc(ctx2)->resolve<IRegisteredContextService>("contract_test") == nullptr,
           "resetAll clears a registered non-zero context export");
    expect(ctx0Inj() == nullptr && ctx2Inj() == nullptr,
           "already-resolved context injections report null after resetAll");
    expect(globalImpl.use_count() == 1 && contextImpl.use_count() == 1 && ctx2Impl.use_count() == 1,
           "resetAll releases the registered service instances");

    // 7. A required dependency that is gone fails instead of serving a stale
    //    adapter. The host registers its native filesystem exactly like this
    //    (native/noqt_runtime.cpp) and guards later use of it with
    //    native/engine_audio.cpp:464-466 (that guard itself runs in section 8).
    auto runtimeFileSystem = std::make_shared<music::NativeFileSystem>(assetsRoot);
    gIoc->registerExport<muse::io::IFileSystem>("contract_test", runtimeFileSystem);
    expect(gIoc->resolve<muse::io::IFileSystem>("contract_test") == runtimeFileSystem,
           "the required IFileSystem export resolves");
    expect(bool(gIoc->resolve<muse::io::IFileSystem>("contract_test")->exists(":/test.txt")),
           "the resolved required adapter is functional");

    GlobalInject<muse::io::IFileSystem> requiredInj;
    expect(requiredInj() == runtimeFileSystem, "a required injection resolves the host adapter");

    resetAll();
    expect(gIoc->resolve<muse::io::IFileSystem>("contract_test") == nullptr,
           "a required global export is unresolvable after resetAll");
    expect(requiredInj() == nullptr,
           "a resolved required injection reports null after resetAll, so the host guard "
           "takes its failure path instead of reusing a cleared adapter");
    expect(runtimeFileSystem.use_count() == 1, "resetAll releases the required export");

    // Leave no registry registrations behind for any later use of the process IoC.
    removeIoC(ctx2);
    resetAll();

    std::cout << "IoC modularity contract checks passed\n";
}

/*
 * 8. Host failure path. initialize_engine_audio() resolves IFileSystem itself
 *    (native/engine_audio.cpp:464-466) because it turns the SoundFont path into
 *    an absolute Uri, and that path ends in FileInfo's own GlobalInject, which
 *    would be a null dereference (muse/framework/global/io/fileinfo.h:33,
 *    fileinfo.cpp:71). With the service gone the call must report that clearly
 *    instead, and the process must stay in the documented "no engine" state
 *    instead of a half-built one.
 */
void test_required_service_host_failure(const std::filesystem::path& root)
{
    using namespace kors::modularity;

    resetAll();
    expect(globalIoc()->resolve<muse::io::IFileSystem>("engine_audio") == nullptr,
           "no file-system service is registered before the host call");

    // Control: the entry point is live and its own argument checks run first, so
    // the failure below is the service guard and not a dead or no-op call.
    const std::string notASoundFont = utf8String(root / "missing.wav");
    std::string error;
    expect(!music::initialize_engine_audio(notASoundFont, &error),
           "engine audio rejects a non-SoundFont path");
    expect(error == "SoundFont must be an existing .sf2 or .sf3 file: " + notASoundFont,
           "the rejected path reports the documented SoundFont argument error");

    // The guard: the required file-system service is not registered.
    error.clear();
    const std::string soundfontPath = utf8String(root / "missing.sf2");
    expect(!music::initialize_engine_audio(soundfontPath, &error),
           "engine audio fails instead of using a missing file-system service");
    expect(error == "native runtime (IFileSystem) is not initialised; "
                    "call initialize_native_runtime() first",
           "the missing service reports the documented IFileSystem error");
    expect(!music::engine_audio_initialized(), "the failed call leaves no engine behind");

    // The next required entry point sees the same state, so a refused
    // initialisation is not mistaken for a usable engine.
    const music::AudioRenderResult result = music::export_score_audio(nullptr, music::AudioRenderRequest{});
    expect(!result.ok, "audio export fails without an initialised engine");
    expect(result.error == "audio engine is not initialised",
           "audio export reports the documented not-initialised error");

    std::cout << "required-service host failure checks passed\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected a fresh test directory");
        const auto root = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        expect(!std::filesystem::exists(root), "Test directory already exists");
        std::filesystem::create_directories(root / "assets");
        std::ofstream(root / "assets" / "test.txt", std::ios::binary) << "resource";
        music::NativeFileSystem fs(root / "assets");
        auto asPath = [](const std::filesystem::path& p) {
            auto value = p.u8string();
            return muse::io::path_t(std::string(value.begin(), value.end()));
        };
        expect(bool(fs.readFile(":/test.txt").ret), "Resource read");
        expect(bool(fs.readFile("://test.txt").ret), "Resource read with a doubled slash");
        expect(!fs.readFile("://../outside.txt").ret, "Doubled-slash traversal rejected");
        expect(!fs.readFile(":/../outside.txt").ret, "Resource traversal rejected");
        expect(!fs.writeFile(":/test.txt", muse::ByteArray("bad", 3)), "Resource write rejected");
        expect(!fs.writeFile(asPath(root / "assets/test.txt"), muse::ByteArray("bad", 3)), "Absolute resource write rejected");
        const auto file = asPath(root / std::filesystem::u8path("测试.bin"));
        expect(bool(fs.writeFile(file, muse::ByteArray("abcdef", 6))), "Unicode write");
        auto data = fs.readFile(file);
        expect(bool(data.ret) && data.val == muse::ByteArray("abcdef", 6), "Read/write content");
        auto stream = fs.openStream(file, muse::io::OpenMode::WriteOnly);
        expect(bool(stream.ret), "Open stream");
        expect(bool(fs.writeToStream(stream.val, muse::ByteArray("abcd", 4))), "Write stream");
        expect(bool(fs.writeToStream(stream.val, muse::ByteArray("XY", 2), 1)), "Seek overwrite");
        expect(bool(fs.closeStream(stream.val)), "Close stream");
        expect(fs.readFile(file).val == muse::ByteArray("aXYd", 4), "Stream content");
        expect(!fs.closeStream(stream.val), "Stale stream rejected");
        const auto scan = fs.scanFiles(asPath(root), {"*.bin"}, muse::io::ScanMode::FilesInCurrentDir);
        expect(bool(scan.ret) && scan.val.size() == 1, "Scan filter");
        auto* settings = muse::Settings::instance();
        const muse::Settings::Key key("test", "value"), added("test", "added");
        settings->setDefaultValue(key, muse::Val(1));
        settings->beginTransaction();
        settings->setLocalValue(key, muse::Val(2));
        settings->setDefaultValue(added, muse::Val(3));
        settings->setDescription(added, "new default");
        settings->commitTransaction();
        expect(settings->value(key).toInt() == 2 && settings->defaultValue(added).toInt() == 3,
               "Commit preserves new default metadata");
        settings->beginTransaction();
        settings->setLocalValue(key, muse::Val(4));
        settings->rollbackTransaction();
        expect(settings->value(key).toInt() == 2, "Rollback");
        settings->reset();
        expect(settings->value(key).toInt() == 1, "Reset defaults");
        test_ioc_contract(root / "assets");
        test_required_service_host_failure(root);
        std::cout << "native filesystem/settings checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
