#include "noqt_runtime.h"
#include "noqt_application.h"
#include "noqt_filesystem.h"
#include "engrave/src/score/score_metadata.h"
#include "engrave/src/score/score_excerpts.h"
#include "engrave/src/io/score_imports.h"
#include "engrave/src/io/midi_import_adapter.h"
#include "runtime_assets.h"
#include "engraving/rw/rwregister.h"
#include "engrave/src/score/score_positions.h"
#include "engrave/src/render/score_render.h"
#include "global/log.h"
#include "global/io/buffer.h"
#include "global/serialization/zipreader.h"
#include "global/serialization/zipwriter.h"
#include "global/serialization/xmlstreamreader.h"
#include "draw/internal/fontprovider.h"
#include "draw/internal/fontsengine.h"
#include "draw/internal/fontsdatabase.h"
#include "engraving/engravingmodule.h"
#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/score.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/instrtemplate.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/part.h"
#include "engraving/infrastructure/localfileinfoprovider.h"
#include "engraving/rw/inoutdata.h"
#include "mpe/mpemodule.h"
#include "importexport/musicxml/internal/import/importmusicxml.h"
#include "importexport/musicxml/internal/export/exportmusicxml.h"
#include "importexport/musicxml/internal/musicxmlconfiguration.h"
#include "importexport/midi/internal/midiexport/exportmidi.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace music {
using namespace muse;
using namespace mu;
namespace {
class StderrLog final : public logger::LogDest {
public:
    StderrLog() : LogDest(logger::LogLayout("${type}: ${message}")) {}
    std::string name() const override { return "native-stderr"; }
    void write(const logger::LogMsg& message) override { std::cerr << m_layout.output(message) << '\n'; }
};


void check(const Ret& result) { if (!result) throw std::runtime_error(result.toString()); }
std::string bytes(const ByteArray& data) { return {reinterpret_cast<const char*>(data.constData()), data.size()}; }
ByteArray byte_array(const std::string& data) { return ByteArray(data.data(), data.size()); }

void check_zip(ZipReader& zip) {
    uint64_t total = 0;
    const auto entries = zip.fileInfoList();
    if (entries.empty() || entries.size() > 4096) throw std::invalid_argument("Invalid archive entry count");
    for (const auto& entry : entries) {
        if (entry.size > 64 * 1024 * 1024 || total + entry.size > 128 * 1024 * 1024)
            throw std::invalid_argument("Uncompressed archive exceeds size limit");
        total += entry.size;
    }
    if (zip.hasError()) throw std::invalid_argument("Invalid archive");
}

// Context-scoped module setups (modularity::IContextSetup) belong to the
// process-wide context and are never destroyed while the engine lives.
std::vector<std::unique_ptr<modularity::IContextSetup> > s_contextSetups;

// The Qt-free part of AppFactory::addConsoleModules(): the framework "mpe"
// module, then the engraving module, then the import/export modules. DrawModule
// is not instantiated: the host installs its own font database/engine through
// install_runtime_font_services() (retained runtime fonts, 1200 DPI provider),
// and DrawModule::registerExports() would fill the same IoC slots with the plain
// FT ones. "audio" is registered by engine_audio.cpp through EngineGlobalSetup,
// and "midi" has no compiled module (the MIDI import/export path is a direct
// function call).
void setup_modules(const std::vector<modularity::IModuleSetup*>& modules,
                   const std::shared_ptr<iex::musicxml::MusicXmlConfiguration>& musicXml,
                   const IApplication::RunMode runMode)
{
    // Phase order and grouping follow BaseApplication::doSetup(): every module
    // registers its resources and exports first, only then imports are resolved,
    // then onPreInit -> onInit -> onAllInited run over the whole list.
    for (auto* module : modules) module->registerResources();
    for (auto* module : modules) module->registerExports();

    for (auto* module : modules) module->resolveImports();

    for (auto* module : modules) module->onPreInit(runMode);

    for (auto* module : modules) module->onInit(runMode);
    // MusicXmlModule::onInit() does exactly this. MUSICXML_NO_INTERNAL leaves that
    // module empty, so the host creates and initialises the configuration itself.
    if (musicXml) musicXml->init();

    for (auto* module : modules) module->onAllInited(runMode);
}

// BaseApplication::setupContext(): the context-scoped setups, in the same phase
// order as the global ones. With engraving internals compiled in this registers
// the palette score provider, the context configuration and the elements
// provider; under ENGRAVING_NO_INTERNAL every EngravingContext hook is empty.
void setup_context_modules(const std::vector<modularity::IModuleSetup*>& modules,
                           const modularity::ContextPtr& ctx,
                           const IApplication::RunMode runMode)
{
    std::vector<modularity::IContextSetup*> setups;
    for (auto* module : modules) {
        if (auto* setup = module->newContext(ctx)) {
            s_contextSetups.emplace_back(setup);
            setups.push_back(setup);
        }
    }
    for (auto* setup : setups) setup->registerExports();
    for (auto* setup : setups) setup->resolveImports();
    for (auto* setup : setups) setup->onPreInit(runMode);
    for (auto* setup : setups) setup->onInit(runMode);
    for (auto* setup : setups) setup->onAllInited(runMode);
}

void finish_load(engraving::EngravingProject& project) {
    auto* score = project.masterScore();
    score->connectTies();
    // 5.0 moved the post-load MIDI wiring out of EngravingProject::setupMasterScore
    // into the MSCX reader (rw/read500/read500.cpp:307-315), so a score that did not
    // come from MSCX keeps an empty MasterScore::midiMapping(): every InstrChannel
    // still has channel() == -1 (dom/instrument.cpp:319) and
    // MasterScore::playbackChannel() indexes m_midiMapping with it
    // (dom/masterscore.h:185), i.e. reads 72*(channel()+1) bytes out of bounds.
    // Run the same three steps, in the same order, where every reader converges.
    for (auto* part : score->parts()) part->updateHarmonyChannels(false);
    score->rebuildMidiMapping();
    score->updateChannel();
    check(project.setupMasterScore(true));
    // MusicXML import can call Part::setMidiProgram after the MIDI mapping was
    // built, leaving playback channels on the template program until an MSCZ
    // reload rebuilds them. Sync now so playback is stable across reloads.
    for (auto* part : score->parts()) {
        for (const auto& pair : part->instruments()) {
            for (auto* channel : pair.second->channel()) {
                if (auto* playback = score->playbackChannel(channel)) {
                    playback->setProgram(channel->program());
                    playback->setBank(channel->bank());
                }
            }
        }
    }
    score->setLayoutAll();
    score->update();
}
}

void initialize_native_runtime(const std::string& assets) {
    static bool initialized = false;
    if (initialized) throw std::runtime_error("Only one engine instance per process is supported");
    const auto root = std::filesystem::u8path(assets);
    if (!std::filesystem::is_regular_file(root / "engraving/instruments/instruments.xml"))
        throw std::invalid_argument("Missing native resources: " + assets);
    // Once IoC mutation begins, a failed initialization cannot safely be retried.
    initialized = true;
    auto* logger = logger::Logger::instance();
    logger->clearDests();
    logger->addDest(new StderrLog());

    const IApplication::RunMode runMode = IApplication::RunMode::ConsoleApp;

    // Host-owned exports come first, like BaseApplication::doSetup() registering
    // IApplication before any module's exports.
    modularity::globalIoc()->registerExport<io::IFileSystem>("native", new NativeFileSystem(root));
    install_runtime_font_services();
    modularity::globalIoc()->registerExport<draw::IFontProvider>("native", std::make_shared<draw::FontProvider>(modularity::globalCtx()));
    // Writer, MusicXML export, and the $v header macro resolve IApplication
    // through the project IoC; without it application() is null and export crashes.
    auto application = std::make_shared<NoQtApplication>();
    modularity::globalIoc()->registerExport<IApplication>("native", application);

    // Module services live in the process-wide IoC; no desktop application is created.
    mpe::MpeModule mpe;
    engraving::EngravingModule engraving;
    std::vector<modularity::IModuleSetup*> modules{ &mpe, &engraving };
    auto musicXml = std::make_shared<iex::musicxml::MusicXmlConfiguration>();
    modularity::globalIoc()->registerExport<iex::musicxml::IMusicXmlConfiguration>("native", musicXml);

    setup_modules(modules, musicXml, runMode);

    // BaseApplication::setupNewContext() creates the context and runs the context
    // setups after the global ones; NoQtApplication's single context is globalCtx.
    const modularity::ContextPtr ctx = application->setupNewContext();
    setup_context_modules(modules, ctx, runMode);

    engraving::loadInstrumentTemplates(":/engraving/instruments/instruments.xml");
}

void apply_score_versions(engraving::MasterScore* score, const std::string& version,
                          int revision, int msc_version) {
    if (!score) throw std::invalid_argument("Score version restore requires a master score");
    score->setMscoreVersion(String::fromStdString(version));
    score->setMscoreRevision(revision);
    score->setMscVersion(msc_version);
}

void copy_score_versions(engraving::MasterScore* source, engraving::MasterScore* target) {
    if (!source || !target)
        throw std::invalid_argument("Score version copy requires two master scores");
    apply_score_versions(target, source->mscoreVersion().toStdString(),
                         source->mscoreRevision(), source->mscVersion());
}

ScoreProject load_native_score(const std::string& input, const std::string& format) {
    if (input.empty() || input.size() > 8 * 1024 * 1024) throw std::invalid_argument("Input must contain 1..8388608 bytes");
    // Like NotationProject::doImport (project/internal/notationproject.cpp:304) and
    // importMusicXmlfromBuffer, the whole reading + setup phase is a load: layout
    // inside it runs outside a transaction and must not push undo commands or warn.
    engraving::ScoreLoad scoreLoad;
    auto project = engraving::EngravingProject::create(modularity::globalCtx());
    project->setFileInfoProvider(std::make_shared<engraving::LocalFileInfoProvider>("Untitled.mscz"));
    ByteArray data = byte_array(input);
    if (format == "mxl" || format == "mscz") {
        io::Buffer buffer(&data);
        buffer.open(io::IODevice::ReadOnly);
        ZipReader zip(&buffer);
        check_zip(zip);
        if (format == "mxl") {
            XmlStreamReader xml(zip.fileData("META-INF/container.xml"));
            std::string root;
            while (!xml.atEnd()) {
                xml.readNext();
                if (root.empty() && xml.isStartElement() && xml.name() == "rootfile") {
                    root = xml.attribute("full-path").toStdString();
                }
            }
            if (xml.error() != XmlStreamReader::NoError) throw std::invalid_argument("Malformed MXL container.xml");
            if (root.empty()) throw std::invalid_argument("MXL has no rootfile");
            data = zip.fileData(root);
            if (zip.hasError() || data.empty() || data.size() > 8 * 1024 * 1024)
                throw std::invalid_argument("MXL rootfile must contain 1..8388608 bytes");
        }
    }
    if (format == "mscz" || format == "mscx") {
        ByteArray archive;
        if (format == "mscx") {
            io::Buffer out(&archive);
            out.open(io::IODevice::WriteOnly);
            ZipWriter zip(&out);
            zip.addFile("score.mscx", data);
            zip.close();
            data = archive;
        }
        io::Buffer buffer(&data);
        engraving::MscReader::Params params;
        params.device = &buffer;
        params.mode = engraving::MscIoMode::Zip;
        engraving::MscReader reader(params);
        check(reader.open());
        if (!reader.imageFileNames().empty()) throw std::invalid_argument("Embedded score images are not yet supported by the native backend");
        engraving::rw::ReadInOutData inout;
        inout.forcePageMode = true;
        const auto loaded = project->loadMscz(reader, &inout, false);
        if (loaded.code() == static_cast<int>(engraving::Err::FileTooOld))
            throw UnsupportedScoreVersion(loaded.toString());
        check(loaded);
    } else if (format == "musicxml" || format == "xml" || format == "mxl") {
        XmlStreamReader scan(data);
        while (!scan.atEnd()) {
            scan.readNext();
            if (scan.isStartElement() && scan.name() == "image")
                throw std::invalid_argument("Embedded score images are not yet supported by the native backend");
        }
        if (scan.error() != XmlStreamReader::NoError) throw std::invalid_argument("Malformed MusicXML");
        if (iex::musicxml::importMusicXmlfromBuffer(project->masterScore(), u"input.musicxml", data) != engraving::Err::NoError)
            throw std::runtime_error("MusicXML import failed");
    } else if (format == "mid" || format == "midi" || format == "kar") {
        if (import_midi_score(project->masterScore(), input) != engraving::Err::NoError)
            throw std::runtime_error("MIDI import failed");
    } else if (format == "gp" || format == "gp3" || format == "gp4" || format == "gp5"
               || format == "gp6" || format == "gp7" || format == "gpx" || format == "gtp" || format == "ptb") {
        import_guitarpro_score(project->masterScore(), input);
    } else throw std::invalid_argument("Unsupported score input format");
    finish_load(*project);
    return project;
}

ScoreProject load_native_score_dir(const std::string& mscx_path) {
    engraving::ScoreLoad scoreLoad;
    auto project = engraving::EngravingProject::create(modularity::globalCtx());
    project->setFileInfoProvider(std::make_shared<engraving::LocalFileInfoProvider>("Untitled.mscz"));
    engraving::MscReader::Params params;
    params.filePath = io::path_t(mscx_path);
    params.mode = engraving::MscIoMode::Dir;
    engraving::MscReader reader(params);
    check(reader.open());
    engraving::rw::ReadInOutData inout;
    inout.forcePageMode = true;
    check(project->loadMscz(reader, &inout, false));
    finish_load(*project);
    return project;
}

std::string export_native_score(const ScoreProject& project, const std::string& format, int page, const std::string& soundfont) {
    ExportOptions options;
    options.page = page;
    return export_native_score(project, format, options, soundfont);
}

std::string export_native_score(const ScoreProject& project, const std::string& format,
                                const ExportOptions& options, const std::string& soundfont) {
    // Audio formats are rendered by the worker's MuseScore engine; the
    // SoundFont parameter is retained for source compatibility.
    (void)soundfont;
    auto* score = select_score(project->masterScore(), options.excerpt_id);
    if (format == "metadata") return score_metadata(score).dump();
    if (format == "measurePositions" || format == "segmentPositions")
        return score_positions(score, format == "segmentPositions").dump();
    if (format == "svg" || format == "png" || format == "pdf") return render_score(score, format, options);

    ByteArray data;
    io::Buffer buffer(&data);
    buffer.open(io::IODevice::WriteOnly);
    bool success = false;
    if (format == "musicxml") success = iex::musicxml::saveXml(score, &buffer);
    else if (format == "mxl") success = iex::musicxml::saveMxl(score, &buffer);
    else if (format == "mscz" || format == "mscx") {
        // Writer::writeScore stamps the writing program's version into every score it
        // writes; an export is read-only for the document, so restore the file's values
        // (metadata mscoreVersion, $v on unedited scores, legacy layout by mscVersion).
        struct VersionFields {
            engraving::Score* score;
            String version;
            int revision;
            int msc;
        };
        struct RestoreVersions {
            std::vector<VersionFields> fields;
            ~RestoreVersions() {
                for (const auto& f : fields) {
                    f.score->setMscoreVersion(f.version);
                    f.score->setMscoreRevision(f.revision);
                    f.score->setMscVersion(f.msc);
                }
            }
        } restore_versions;
        auto* master = project->masterScore();
        restore_versions.fields.push_back({master, master->mscoreVersion(), master->mscoreRevision(), master->mscVersion()});
        for (engraving::Excerpt* excerpt : master->excerpts()) {
            if (auto* part = excerpt->excerptScore())
                restore_versions.fields.push_back({part, part->mscoreVersion(), part->mscoreRevision(), part->mscVersion()});
        }
        engraving::MscWriter::Params params;
        params.device = &buffer;
        params.mainFileName = u"score.mscx";
        engraving::MscWriter writer(params);
        check(writer.open());
        if (options.excerpt_id == -1) {
            success = project->writeMscz(writer, false);
        } else {
            // Same upstream writer/style logic as MscSaver::exportPart, without a Qt thumbnail.
            const auto original_tags = score->metaTags();
            struct RestoreTags {
                engraving::Score* score;
                std::map<String, String> tags;
                ~RestoreTags() { score->setMetaTags(tags); }
            } restore{score, original_tags};
            auto effective_tags = project->masterScore()->metaTags();
            for (const auto& [key, value] : original_tags) effective_tags.insert_or_assign(key, value);
            score->setMetaTags(effective_tags);
            ByteArray style_data, score_data;
            io::Buffer style_buffer(&style_data), score_buffer(&score_data);
            style_buffer.open(io::IODevice::WriteOnly);
            score_buffer.open(io::IODevice::WriteOnly);
            score->style().write(&style_buffer);
            success = engraving::rw::RWRegister::writer()->writeScore(score, &score_buffer);
            writer.writeStyleFile(style_data);
            writer.writeScoreFile(score_data);
        }
        writer.close();
        success = success && !writer.hasError();
        if (format == "mscx" && success) {
            buffer.close();
            ByteArray msczData = std::move(data);
            data = ByteArray();
            io::Buffer msczBuf(&msczData);
            engraving::MscReader::Params rparams;
            rparams.device = &msczBuf;
            rparams.mode = engraving::MscIoMode::Zip;
            engraving::MscReader reader(rparams);
            check(reader.open());
            data = reader.readScoreFile();
            reader.close();
            success = !data.empty();
        }
    } else if (format == "midi") {
        iex::midi::ExportMidi writer(score);
        success = writer.write(&buffer, options.midi_expand_repeats, options.midi_export_rpns);
    } else throw std::invalid_argument("Unsupported export format");
    buffer.close();
    if (!success || data.empty()) throw std::runtime_error("Score export failed");
    return bytes(data);
}
}
