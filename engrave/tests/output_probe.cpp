// Renders one score through the public API and writes the results, so the
// superproject's scripts/test_a4_output.py can compare them with the A2 goldens
// exactly as scripts/golden.py does.
//
//   engrave_output_probe --assets=DIR --soundfont=FILE... --render=SCORE --out=DIR
//   engrave_output_probe --assets=DIR --soundfont=FILE... --audio=SCORE --format=wav|flac|ogg|mp3|aac
//                        --out=FILE [--repeats=0] [--sample-rate=HZ]
//
// --render writes page-<n>.svg for every page, page-<n>.png for every page and
// score.pdf, all with the A2 defaults (144 dpi, transparent PNG, no page
// background). It prints one JSON object on stdout. The exit status is 0 on
// success, 2 for a returned error (printed as "error: Code: message") and 3 for
// an exception.

#include <engrave/engrave.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string_view extension_of(engrave::AudioFormat format) {
    switch (format) {
    case engrave::AudioFormat::Wav: return "wav";
    case engrave::AudioFormat::Flac: return "flac";
    case engrave::AudioFormat::Opus: return "ogg";
    case engrave::AudioFormat::Mp3: return "mp3";
    case engrave::AudioFormat::Aac: return "aac";
    }
    return "bin";
}

void write(const std::filesystem::path& path, const std::vector<std::byte>& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) throw std::runtime_error("cannot write '" + path.string() + "'");
}

void write_audio(const std::filesystem::path& path, const std::vector<std::byte>& data) {
    write(path, data);
}

int usage() {
    std::fprintf(stderr, "usage: --assets=DIR [--soundfont=FILE]... --render=SCORE --out=DIR\n"
                         "       --assets=DIR [--soundfont=FILE]... --audio=SCORE --format=NAME --out=FILE\n");
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path assets, out, score;
    std::vector<std::filesystem::path> soundfonts;
    std::string mode, format = "wav";
    bool repeats = true;
    int sample_rate = 44100;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto value = [&argument](std::size_t prefix) { return std::string(argument.substr(prefix)); };
        if (argument.starts_with("--assets=")) assets = std::filesystem::u8path(value(9));
        else if (argument.starts_with("--soundfont=")) soundfonts.push_back(std::filesystem::u8path(value(12)));
        else if (argument.starts_with("--out=")) out = std::filesystem::u8path(value(6));
        else if (argument.starts_with("--render=")) { mode = "render"; score = std::filesystem::u8path(value(9)); }
        else if (argument.starts_with("--audio=")) { mode = "audio"; score = std::filesystem::u8path(value(8)); }
        else if (argument.starts_with("--format=")) format = value(9);
        else if (argument.starts_with("--sample-rate=")) sample_rate = std::atoi(value(14).c_str());
        else if (argument == "--no-repeats") repeats = false;
        else return usage();
    }
    if (assets.empty() || score.empty() || out.empty() || mode.empty()) return usage();

    try {
        engrave::EngineOptions options;
        options.resources = assets;
        options.soundfonts = soundfonts;
        options.audio_sample_rate = sample_rate;
        auto engine = engrave::Engine::create(options);
        if (!engine) {
            std::fprintf(stderr, "error: %s: %s\n", std::string(engrave::to_string(engine.error().code)).c_str(),
                         engine.error().message.c_str());
            return 2;
        }
        auto document = (*engine)->open(score);
        if (!document) {
            std::fprintf(stderr, "error: %s: %s\n", std::string(engrave::to_string(document.error().code)).c_str(),
                         document.error().message.c_str());
            return 2;
        }

        if (mode == "render") {
            engrave::RenderOptions render;
            render.format = engrave::RenderFormat::Svg;
            const auto svg = (*document)->render(render);
            if (!svg) {
                std::fprintf(stderr, "error: %s: %s\n", std::string(engrave::to_string(svg.error().code)).c_str(),
                             svg.error().message.c_str());
                return 2;
            }
            std::filesystem::create_directories(out);
            for (std::size_t page = 0; page < svg->pages.size(); ++page)
                write(out / ("page-" + std::to_string(page) + ".svg"), svg->pages[page]);

            render.format = engrave::RenderFormat::Png;
            const auto png = (*document)->render(render);
            if (!png) {
                std::fprintf(stderr, "error: %s: %s\n", std::string(engrave::to_string(png.error().code)).c_str(),
                             png.error().message.c_str());
                return 2;
            }
            for (std::size_t page = 0; page < png->pages.size(); ++page)
                write(out / ("page-" + std::to_string(page) + ".png"), png->pages[page]);

            render.format = engrave::RenderFormat::Pdf;
            const auto pdf = (*document)->render(render);
            if (!pdf) {
                std::fprintf(stderr, "error: %s: %s\n", std::string(engrave::to_string(pdf.error().code)).c_str(),
                             pdf.error().message.c_str());
                return 2;
            }
            write(out / "score.pdf", pdf->document);
            std::printf("{\"pages\": %zu}\n", svg->pages.size());
            return 0;
        }

        engrave::AudioOptions audio;
        audio.use_repeats = repeats;
        if (format == "wav") audio.format = engrave::AudioFormat::Wav;
        else if (format == "flac") audio.format = engrave::AudioFormat::Flac;
        else if (format == "ogg" || format == "opus") audio.format = engrave::AudioFormat::Opus;
        else if (format == "mp3") audio.format = engrave::AudioFormat::Mp3;
        else if (format == "aac") audio.format = engrave::AudioFormat::Aac;
        else return usage();
        audio.sample_rate = sample_rate;

        std::vector<std::byte> bytes;
        const auto rendered = (*document)->render_audio(audio, [&bytes](std::span<const std::byte> chunk) {
            bytes.insert(bytes.end(), chunk.begin(), chunk.end());
        });
        if (!rendered) {
            std::fprintf(stderr, "error: %s: %s\n", std::string(engrave::to_string(rendered.error().code)).c_str(),
                         rendered.error().message.c_str());
            return 2;
        }
        write_audio(out, bytes);
        std::printf("{\"bytes\": %llu, \"frames\": %llu, \"seconds\": %.3f, \"extension\": \"%s\"}\n",
                    static_cast<unsigned long long>(rendered->bytes), static_cast<unsigned long long>(rendered->frames),
                    rendered->seconds, std::string(extension_of(audio.format)).c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "exception: %s\n", error.what());
        return 3;
    }
}
