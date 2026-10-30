// An engrave client that knows nothing but the public API: it is compiled
// with engrave/include as its only include path (xmake/tests.lua).
//
//   example_embedder <resources> <score file>...
//
// Opens every score file, creates a score from every template, and exports
// each of them to MSCZ, MusicXML, MXL and MIDI; every export is opened again.
// Exits non-zero on the first failure.

#include <cstdio>
#include <filesystem>
#include <string>

#include <engrave/engrave.h>

namespace {

bool report(const engrave::Error& error, const std::string& what) {
    std::fprintf(stderr, "FAIL %s: %.*s: %s\n", what.c_str(), static_cast<int>(to_string(error.code).size()),
                 to_string(error.code).data(), error.message.c_str());
    return false;
}

bool exercise(engrave::Engine& engine, engrave::Document& document, const std::string& name) {
    for (const auto format : {engrave::Format::Mscz, engrave::Format::MusicXml, engrave::Format::Mxl, engrave::Format::Midi}) {
        const std::string what = name + " -> " + std::string(to_string(format));
        const auto bytes = document.export_score({.format = format});
        if (!bytes) return report(bytes.error(), what);
        const auto reopened = engine.open(*bytes);
        if (!reopened) return report(reopened.error(), what + " -> open");
        std::printf("ok %s (%zu bytes, reopened)\n", what.c_str(), bytes->size());
    }
    if (document.revision() != 1) {
        std::fprintf(stderr, "FAIL %s: revision %llu after export\n", name.c_str(),
                     static_cast<unsigned long long>(document.revision()));
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: example_embedder <resources> <score file>...\n");
        return 2;
    }
    auto engine = engrave::Engine::create({.resources = argv[1]});
    if (!engine) {
        report(engine.error(), "Engine::create");
        return 1;
    }

    const auto capabilities = (*engine)->capabilities();
    std::printf("engrave %.*s, engine %s, %zu import and %zu export formats\n",
                static_cast<int>(engrave::version_string.size()), engrave::version_string.data(),
                capabilities.engine_version.c_str(), capabilities.import_formats.size(),
                capabilities.export_formats.size());

    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        const std::filesystem::path file = argv[i];
        const auto document = (*engine)->open(file);
        if (!document) {
            failures += !report(document.error(), file.string());
            continue;
        }
        for (const auto& diagnostic : (*document)->diagnostics())
            std::printf("note %s: %s\n", file.string().c_str(), diagnostic.message.c_str());
        failures += !exercise(**engine, **document, file.filename().string());
    }

    const auto templates = (*engine)->templates();
    if (templates.empty()) {
        std::fprintf(stderr, "FAIL no templates in %s\n", argv[1]);
        ++failures;
    }
    for (const auto& info : templates) {
        const auto document = (*engine)->create_document({.template_id = info.id, .title = info.title, .composer = "engrave"});
        if (!document) {
            failures += !report(document.error(), "template " + info.id);
            continue;
        }
        failures += !exercise(**engine, **document, info.category + "/" + info.id);
    }

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
