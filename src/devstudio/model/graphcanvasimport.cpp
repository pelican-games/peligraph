#include "graphcanvasimport.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string utf8(const std::filesystem::path &path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

bool readJson(const std::filesystem::path &file, Json &into, std::string &error) {
    std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        error = "Could not read " + utf8(file.filename()) + ".";
        return false;
    }
    std::stringstream text;
    text << stream.rdbuf();
    try {
        into = Json::parse(text.str());
    } catch (const std::exception &failure) {
        error = utf8(file.filename()) + " is not JSON: " + failure.what();
        return false;
    }
    return true;
}

// `project://imports/<directory>/...` when every image asset of the
// definition names the same one, else empty.
std::string assetDirectory(const Json &definition) {
    constexpr std::string_view imports = "project://imports/";
    std::string directory;
    const auto nodes = definition.find("nodes");
    if (nodes == definition.end() || !nodes->is_array()) return {};
    for (const auto &node : *nodes) {
        if (!node.is_object() || node.value("op", std::string{}) != "canvas.image") continue;
        const auto asset = node.value("parameters", Json::object()).value("asset", std::string{});
        if (!asset.starts_with(imports)) return {};
        const auto slash = asset.find('/', imports.size());
        if (slash == std::string::npos) return {};
        const auto named = asset.substr(imports.size(), slash - imports.size());
        if (!directory.empty() && directory != named) return {};
        directory = named;
    }
    return directory;
}

} // namespace

bool isCanvasImageFile(const std::filesystem::path &file) {
    const auto extension = lowered(file.extension().string());
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg";
}

GraphCanvasPictureImport importCanvasImages(const std::filesystem::path &project_root,
                                          const std::vector<std::filesystem::path> &files) {
    GraphCanvasPictureImport result;
    if (project_root.empty()) {
        result.error = "No project is open.";
        return result;
    }
    const auto folder = project_root / "assets" / "canvas";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        result.error = "Could not make " + utf8(folder) + ": " + error.message();
        return result;
    }
    for (const auto &file : files) {
        if (!isCanvasImageFile(file)) {
            result.error = utf8(file.filename()) + " is not a .png or .jpg file.";
            continue;
        }
        const auto stem = utf8(file.stem());
        const auto extension = utf8(file.extension());
        auto name = stem + extension;
        for (int suffix = 2; std::filesystem::exists(folder / std::filesystem::u8path(name)) && suffix < 10000;
             ++suffix)
            name = stem + "_" + std::to_string(suffix) + extension;
        const auto target = folder / std::filesystem::u8path(name);
        if (std::filesystem::exists(target)) {
            result.error = "No free name for " + utf8(file.filename()) + ".";
            continue;
        }
        std::filesystem::copy_file(file, target, std::filesystem::copy_options::none, error);
        if (error) {
            result.error = "Could not copy " + utf8(file.filename()) + ": " + error.message();
            error.clear();
            continue;
        }
        result.references.push_back("project://assets/canvas/" + name);
        result.copies.push_back(target);
    }
    return result;
}

GraphCanvasPsdOutput readCanvasPsdOutput(const std::filesystem::path &graph_json) {
    GraphCanvasPsdOutput output;
    Json document;
    if (!readJson(graph_json, document, output.error)) return output;
    const auto definitions = document.find("definitions");
    if (!document.is_object() || definitions == document.end() || !definitions->is_object() ||
        definitions->size() != 1) {
        output.error = utf8(graph_json.filename()) +
                       " is not a psd-canvas output: it should hold exactly one definition.";
        return output;
    }
    const auto first = definitions->begin();
    output.source = graph_json.parent_path();
    output.definition_name = first.key();
    output.definition = first.value();
    output.directory = assetDirectory(output.definition);
    if (output.directory.empty()) output.directory = output.definition_name;
    // The layers: the report's count, else the drawn images.
    std::size_t images = 0;
    for (const auto &node : output.definition.value("nodes", Json::array()))
        if (node.is_object() && node.value("op", std::string{}) == "canvas.image" &&
            node.value("parameters", Json::object()).value("channel", std::string{"rgba"}) != "luminance")
            ++images;
    output.layers = images;
    const auto report_file = output.source / "canvas.report.json";
    if (std::filesystem::exists(report_file)) {
        Json report;
        std::string ignored;
        if (readJson(report_file, report, ignored) && report.is_object()) {
            if (const auto layers = report.find("layers"); layers != report.end() && layers->is_number_unsigned())
                output.layers = layers->get<std::size_t>();
            for (const auto &entry : report.value("diagnostics", Json::array())) {
                if (entry.is_string()) {
                    output.diagnostics.push_back(entry.get<std::string>());
                } else if (entry.is_object()) {
                    // code, then what it is about and why, as the tool writes them.
                    std::string line = entry.value("code", std::string{"note"});
                    for (const auto &[key, value] : entry.items())
                        if (key != "code") line += " " + key + "=" + (value.is_string() ? value.get<std::string>() : value.dump());
                    output.diagnostics.push_back(std::move(line));
                } else {
                    output.diagnostics.push_back(entry.dump());
                }
            }
        }
    }
    return output;
}

std::string copyCanvasPsdOutput(const std::filesystem::path &project_root, const GraphCanvasPsdOutput &output,
                                bool replace) {
    if (project_root.empty()) return "No project is open.";
    const auto target = project_root / "imports" / std::filesystem::u8path(output.directory);
    std::error_code error;
    if (std::filesystem::exists(target)) {
        if (!replace) return "imports/" + output.directory + " is already in the project.";
        std::filesystem::remove_all(target, error);
        if (error) return "Could not replace imports/" + output.directory + ": " + error.message();
    }
    std::filesystem::create_directories(target, error);
    if (error) return "Could not make imports/" + output.directory + ": " + error.message();
    for (const char *file : {"canvas.graph.json", "canvas.report.json"}) {
        const auto from = output.source / file;
        if (!std::filesystem::exists(from)) continue;
        std::filesystem::copy_file(from, target / file, std::filesystem::copy_options::none, error);
        if (error) return std::string{"Could not copy "} + file + ": " + error.message();
    }
    for (const char *folder : {"layers", "masks"}) {
        const auto from = output.source / folder;
        if (!std::filesystem::is_directory(from)) continue;
        std::filesystem::copy(from, target / folder, std::filesystem::copy_options::recursive, error);
        if (error) return std::string{"Could not copy "} + folder + "/: " + error.message();
    }
    return {};
}

// --- SG-3.

bool isCanvasFontFile(const std::filesystem::path &file) {
    const auto extension = lowered(file.extension().string());
    return extension == ".ttf" || extension == ".otf";
}

GraphCanvasFontsRead readCanvasProjectFonts(const std::filesystem::path &project_root) {
    GraphCanvasFontsRead read;
    if (project_root.empty()) {
        read.error = "No project is open.";
        return read;
    }
    std::error_code error;
    std::filesystem::recursive_directory_iterator walk{
        project_root, std::filesystem::directory_options::skip_permission_denied, error};
    if (error) {
        read.error = "The project folder could not be read: " + error.message();
        return read;
    }
    try {
        for (auto end = std::filesystem::recursive_directory_iterator{}; walk != end; walk.increment(error)) {
            if (error) break;
            const auto &entry = *walk;
            std::error_code kind;
            if (entry.is_directory(kind)) {
                const auto name = entry.path().filename().u8string();
                if (!name.empty() && name.front() == u8'.') walk.disable_recursion_pending();
                continue;
            }
            if (!isCanvasFontFile(entry.path())) continue;
            const auto relative = utf8(entry.path().lexically_relative(project_root));
            if (relative.empty() || relative.starts_with("..")) continue;
            read.fonts.push_back("project://" + relative);
        }
    } catch (const std::exception &caught) {
        read.error = std::string{"The project folder could not be read: "} + caught.what();
    }
    std::ranges::sort(read.fonts);
    return read;
}

GraphCanvasFontImport importCanvasFont(const std::filesystem::path &project_root, const std::filesystem::path &file) {
    GraphCanvasFontImport result;
    if (project_root.empty()) {
        result.error = "No project is open.";
        return result;
    }
    if (!isCanvasFontFile(file)) {
        result.error = utf8(file.filename()) + " is not a .ttf or .otf file.";
        return result;
    }
    const auto folder = project_root / "assets" / "fonts";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        result.error = "Could not make " + utf8(folder) + ": " + error.message();
        return result;
    }
    const auto stem = utf8(file.stem());
    const auto extension = utf8(file.extension());
    auto name = stem + extension;
    for (int suffix = 2; std::filesystem::exists(folder / std::filesystem::u8path(name)) && suffix < 10000; ++suffix)
        name = stem + "_" + std::to_string(suffix) + extension;
    const auto target = folder / std::filesystem::u8path(name);
    if (std::filesystem::exists(target)) {
        result.error = "No free name for " + utf8(file.filename()) + ".";
        return result;
    }
    std::filesystem::copy_file(file, target, std::filesystem::copy_options::none, error);
    if (error) {
        result.error = "Could not copy " + utf8(file.filename()) + ": " + error.message();
        return result;
    }
    result.reference = "project://assets/fonts/" + name;
    result.copy = target;
    return result;
}

} // namespace PelicanStudio
