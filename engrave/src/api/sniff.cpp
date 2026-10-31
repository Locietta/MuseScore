#include "sniff.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <nlohmann/json.hpp>

#include "global/io/buffer.h"
#include "global/serialization/xmlstreamreader.h"
#include "global/serialization/zipreader.h"

namespace engrave {
namespace {
bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size()
           && std::equal(suffix.begin(), suffix.end(), text.end() - suffix.size(),
                         [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

std::optional<Format> sniff_zip(const std::string& data) {
    muse::ByteArray bytes = muse::ByteArray::fromRawData(data.data(), data.size());
    muse::io::Buffer buffer(&bytes);
    buffer.open(muse::io::IODevice::ReadOnly);
    muse::ZipReader zip(&buffer);
    if (zip.hasError()) return std::nullopt;
    if (zip.fileExists("META-INF/container.xml")) {
        muse::XmlStreamReader xml(zip.fileData("META-INF/container.xml"));
        bool musicxml = false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement() || xml.name() != "rootfile") continue;
            const std::string path = xml.attribute("full-path").toStdString();
            if (ends_with(path, ".mscx")) return Format::Mscz;
            musicxml = musicxml || ends_with(path, ".musicxml")
                       || (ends_with(path, ".xml") && !path.starts_with("META-INF/"));
        }
        if (musicxml) return Format::Mxl;
    }
    if (zip.fileExists("Content/score.gpif")) return Format::GuitarPro;
    return std::nullopt;
}

std::optional<Format> sniff_xml(const std::string& data) {
    std::string_view text = data;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos || text[first] != '<') return std::nullopt;
    muse::XmlStreamReader xml(muse::ByteArray::fromRawData(data.data(), data.size()));
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == "museScore") return Format::Mscx;
            if (xml.name() == "score-partwise" || xml.name() == "score-timewise") return Format::MusicXml;
            return std::nullopt;
        }
    }
    return std::nullopt;
}
} // namespace

std::optional<Format> sniff_format(const std::string& data) {
    const std::string_view head(data.data(), std::min<std::size_t>(data.size(), 64));
    if (head.starts_with("PK\x03\x04")) {
        if (auto format = sniff_zip(data)) return format;
    }
    std::string_view text = data;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos && text[first] == '{') {
        auto json = nlohmann::json::parse(text, nullptr, false);
        if (json.is_object() && json.contains("mnx") && json["mnx"].is_object()) return Format::Mnx;
    }
    if (head.starts_with("MThd")) return Format::Midi;
    if (auto format = sniff_xml(data)) return format;
    // importGTP's magics: Power Tab, GP6 (BCFZ), and GP1-5, whose header is a
    // length byte followed by "FICHIER GUITAR PRO" or "FICHIER GUITARE PRO".
    if (head.starts_with("ptab") || head.starts_with("BCFZ")
        || (!head.empty() && head.substr(1).starts_with("FICHIER GUITAR")))
        return Format::GuitarPro;
    return std::nullopt;
}

std::optional<Format> format_from_extension(const std::filesystem::path& name) {
    const auto u8 = name.extension().u8string();
    std::string ext(u8.begin(), u8.end());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".mnx") return Format::Mnx;
    if (ext == ".mscz") return Format::Mscz;
    if (ext == ".mscx") return Format::Mscx;
    if (ext == ".musicxml" || ext == ".xml") return Format::MusicXml;
    if (ext == ".mxl") return Format::Mxl;
    if (ext == ".mid" || ext == ".midi" || ext == ".kar") return Format::Midi;
    if (ext == ".gp" || ext == ".gp3" || ext == ".gp4" || ext == ".gp5" || ext == ".gp6" || ext == ".gp7"
        || ext == ".gpx" || ext == ".gtp" || ext == ".ptb")
        return Format::GuitarPro;
    return std::nullopt;
}

} // namespace engrave
