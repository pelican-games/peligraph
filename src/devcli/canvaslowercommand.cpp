#include "canvaslowercommand.hpp"

#include "../core/build_features.hpp"
#include "../project/graphcomposition.hpp"
#if PELICAN_WITH_CANVAS
#include "../project/canvas/canvaslowering.hpp"
#endif

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican::DevCli {
namespace {
namespace fs = std::filesystem;
using OrderedJson = nlohmann::ordered_json;

#if PELICAN_WITH_CANVAS
std::string readText(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void writeText(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << text;
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

// The project, less what belongs to one checkout: its history and the
// engine's caches.
void copyProject(const fs::path &from, const fs::path &to) {
    fs::create_directories(to);
    for (const auto &entry : fs::directory_iterator(from)) {
        const auto name = entry.path().filename().string();
        if (name == ".git" || name == ".pelican") continue;
        fs::copy(entry.path(), to / name,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }
}

// The rendering graph documents project.json names, relative to the project.
std::vector<std::string> renderingDocuments(const OrderedJson &project) {
    std::vector<std::string> documents;
    const auto basic = project.find("basic_config");
    if (basic == project.end() || !basic->is_object()) return documents;
    const auto config = basic->find("rendering_config_json");
    if (config == basic->end()) return documents;
    if (config->is_string()) documents.push_back(config->get<std::string>());
    else if (config->is_array())
        for (const auto &entry : *config)
            if (entry.is_string()) documents.push_back(entry.get<std::string>());
    return documents;
}

int lower(const fs::path &project_dir, const fs::path &out_dir) {
    const auto project_file = project_dir / "project.json";
    const auto project_text = readText(project_file);
    auto project = OrderedJson::parse(project_text);
    const auto source = fs::weakly_canonical(project_dir);
    const auto target = fs::weakly_canonical(out_dir);
    const auto relative = target.lexically_relative(source);
    if (target == source || (!relative.empty() && *relative.begin() != ".."))
        throw std::runtime_error("canvas-lower --out must lie outside the project: " + out_dir.string());

    // Lower every document before writing anything, so a refusal leaves no
    // half-written copy behind.
    struct Lowered {
        std::string path;
        std::string text;
        CanvasLowering lowering;
    };
    std::vector<Lowered> lowered;
    for (const auto &document : renderingDocuments(project)) {
        auto text = readText(project_dir / document);
        auto lowering = lowerCanvasNodes(nlohmann::json::parse(text));
        lowered.push_back({document, std::move(text), std::move(lowering)});
    }

    copyProject(project_dir, out_dir);
    for (const auto &entry : lowered) {
        // A document without a canvas op is copied byte for byte.
        if (!entry.lowering.changed) continue;
        writeText(out_dir / entry.path, entry.lowering.document.dump(2) + "\n");
        for (const auto &generated : entry.lowering.generated_sources)
            writeText(out_dir / generated.path, generated.text);
    }
    if (const auto requires_list = project.find("requires");
        requires_list != project.end() && requires_list->is_array()) {
        auto kept = OrderedJson::array();
        for (const auto &requirement : *requires_list)
            if (requirement != "canvas") kept.push_back(requirement);
        if (kept.size() != requires_list->size()) {
            *requires_list = std::move(kept);
            writeText(out_dir / "project.json", project.dump(2) + "\n");
        }
    }

    OrderedJson report{{"outputs", OrderedJson::array()}, {"diagnostics", OrderedJson::array()}};
    for (const auto &entry : lowered) {
        const auto &lowering = entry.lowering;
        for (std::size_t index = 0; index < lowering.ir.outputs.size(); ++index) {
            const auto &output = lowering.ir.outputs[index];
            std::set<std::uint32_t> kinds;
            for (const auto &item : output.items) kinds.insert(canvasItemKind(item));
            auto kind_names = OrderedJson::array();
            for (const auto kind : kinds) kind_names.push_back(std::string{canvasItemKindName(kind)});
            // Each draw of the output (its groups' first, its own last)
            // generated one vertex and one fragment source.
            auto generated = OrderedJson::array();
            for (const auto &source : lowering.generated_sources)
                if (source.output == output.id && source.stage == "vert") generated.push_back(source.stem);
            OrderedJson row{{"id", output.id}};
            if (!output.scope.empty()) row["scope"] = output.scope;
            row["document"] = entry.path;
            row["items"] = output.items.size();
            if (!output.groups.empty()) row["groups"] = output.groups.size();
            row["kinds"] = std::move(kind_names);
            row["generated"] = std::move(generated);
            report["outputs"].push_back(std::move(row));
        }
        for (const auto &diagnostic : lowering.diagnostics) report["diagnostics"].push_back(diagnostic);
    }
    std::cout << report.dump(2) << std::endl;
    return 0;
}
#endif
} // namespace

int runCanvasLowerCommand(int argc, char *argv[]) {
#if PELICAN_WITH_CANVAS
    argparse::ArgumentParser program("Pelican Cli canvas-lower");
    program.add_argument("--project").required().help("the project directory to lower");
    program.add_argument("--out").required().help("the directory the lowered copy is written to");
    try {
        program.parse_args(argc, argv);
        return lower(program.get<std::string>("--project"), program.get<std::string>("--out"));
    } catch (const GraphRegistryError &error) {
        std::cout << OrderedJson{{"error", {{"code", error.code()}, {"detail", error.detail()}}}}.dump(2) << std::endl;
        std::cerr << error.code() << ": " << error.detail().dump() << std::endl;
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
#else
    (void)argc;
    (void)argv;
    std::cout << OrderedJson{{"error", {{"code", "build_feature_disabled@1"},
                                        {"detail", {{"feature", "PELICAN_WITH_CANVAS"}}}}}}
                     .dump(2)
              << std::endl;
    std::cerr << "build_feature_disabled@1: "
              << buildFeatureDisabledMessage("PELICAN_WITH_CANVAS", "canvas-lower is unavailable") << std::endl;
    return 1;
#endif
}

} // namespace Pelican::DevCli
