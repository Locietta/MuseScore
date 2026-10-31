#pragma once

#include <cstddef>
#include <vector>

#include "global/iapplication.h"

namespace music {
// Qt-free IApplication for the headless host. Version data comes from the
// generated framework config, mirroring BaseApplication. The host runs a single
// engine per process and keeps every service in the process-wide IoC, so its one
// context is the global one (see setupNewContext in the .cpp).
class NoQtApplication final : public muse::IApplication
{
public:
    NoQtApplication() = default;
    ~NoQtApplication() override = default;

    muse::String name() const override;
    muse::String title() const override;

    bool unstable() const override;
    muse::Version version() const override;
    muse::Version fullVersion() const override;
    muse::String build() const override;
    muse::String revision() const override;

    RunMode runMode() const override;
    bool noGui() const override;

    void setup() override;
    void finish() override;
    void restart() override;

    muse::modularity::ContextPtr setupNewContext(const muse::StringList& args = {}) override;
    void destroyContext(const muse::modularity::ContextPtr& ctx) override;
    size_t contextCount() const override;
    std::vector<muse::modularity::ContextPtr> contexts() const override;

    void processEvents() override;
};
}
