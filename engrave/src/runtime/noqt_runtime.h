#pragma once
#include "engrave/src/score/export_options.h"
#include <memory>
#include <string>
#include <stdexcept>

namespace mu::engraving { class EngravingProject; class MasterScore; }
namespace music {
class EngineContext;
struct UnsupportedScoreVersion : std::runtime_error { using std::runtime_error::runtime_error; };
using ScoreProject = std::shared_ptr<mu::engraving::EngravingProject>;
// The first step of EngineContext: validates the resources (std::invalid_argument,
// retryable), then claims the process for its one engine and routes diagnostics
// to stderr.
void initialize_native_runtime(const std::string& assets);
ScoreProject load_native_score(const std::string& bytes, const std::string& format);
// Loads the .mscx of an MSCX directory (score_style.mss and META-INF/ beside
// it, the layout of the score templates) with load_native_score's post-load steps.
ScoreProject load_native_score_dir(const std::string& mscx_path);
// Writer::writeScore stamps the running program's version into the score it
// writes. A serialize/reload of our own writer output therefore has to restore
// the source values, or the reloaded score reports the export as an edit.
void copy_score_versions(mu::engraving::MasterScore* source, mu::engraving::MasterScore* target);
void apply_score_versions(mu::engraving::MasterScore* score, const std::string& version,
                          int revision, int msc_version);
std::string export_native_score(const EngineContext& context, const ScoreProject& project,
                                const std::string& format, int page, const std::string& soundfont);
std::string export_native_score(const EngineContext& context, const ScoreProject& project,
                                const std::string& format, const ExportOptions& options,
                                const std::string& soundfont);
}
