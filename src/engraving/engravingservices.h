/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <memory>

namespace muse::draw {
class IImageProvider;
}

namespace mu::engraving {
class IEngravingConfiguration;
class IEngravingContextConfiguration;
class IEngravingFontsProvider;
namespace rendering {
class IScoreRenderer;
}

// The engine services a score uses. The engine context owns the only instance
// and outlives every score built with it; scores keep a reference.
struct EngravingServices {
    std::shared_ptr<IEngravingConfiguration> configuration;
    std::shared_ptr<IEngravingFontsProvider> engravingFonts;
    std::shared_ptr<IEngravingContextConfiguration> contextConfiguration;
    std::shared_ptr<rendering::IScoreRenderer> renderer;
    // Raster image codec and thumbnails. The engine has none: it rejects scores
    // with raster images when they are opened, so the paths that need it are
    // not reached.
    std::shared_ptr<muse::draw::IImageProvider> imageProvider;
    // Null until the palette score exists (it is built with these services).
};
}
