#include "authoringlowering.hpp"

#if PELICAN_WITH_CANVAS
#include "canvas/canvaslowering.hpp"
#endif

#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <system_error>

namespace Pelican {
namespace {
using Json = nlohmann::json;

#if PELICAN_WITH_CANVAS
// Each output: its items' kinds, which authored nodes drew each instance, and
// the shader pair it generated (in output order, vertex then fragment).
Json canvasReport(const CanvasLowering &lowering) {
    auto outputs = Json::array();
    for (std::size_t index = 0; index < lowering.ir.outputs.size(); ++index) {
        const auto &output = lowering.ir.outputs[index];
        std::set<std::uint32_t> kinds;
        for (const auto &item : output.items) kinds.insert(canvasItemKind(item.shape, item.paint));
        auto kind_names = Json::array();
        for (const auto kind : kinds) kind_names.push_back(std::string{canvasItemKindName(kind)});
        auto items = Json::array();
        for (const auto &item : lowering.items)
            if (item.at("output") == output.id && item.value("scope", std::string{}) == output.scope)
                items.push_back({{"instance", item.at("instance")}, {"kind", item.at("kind")},
                                 {"nodes", item.at("nodes")}});
        auto generated = Json::array();
        for (std::size_t stage = 0; stage < 2; ++stage) {
            const auto &source = lowering.generated_sources.at(index * 2 + stage);
            generated.push_back(
                {{"stem", source.stem}, {"stage", source.stage}, {"path", source.path}, {"text", source.text}});
        }
        Json row{{"id", output.id}};
        if (!output.scope.empty()) row["scope"] = output.scope;
        row["kinds"] = std::move(kind_names);
        row["items"] = std::move(items);
        row["generated"] = std::move(generated);
        outputs.push_back(std::move(row));
    }
    return {{"outputs", std::move(outputs)}, {"diagnostics", lowering.diagnostics}};
}
#endif

std::string readText(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

AuthoringLowering lowerAuthoringDocument(const nlohmann::json &graph_document) {
    AuthoringLowering result;
#if PELICAN_WITH_CANVAS
    // pelican.canvas: one directory per shader pair under the project's own
    // metadata, so a pair's compile cache key never moves with another's.
    auto canvas = lowerCanvasNodes(graph_document, CanvasLoweringOptions{".pelican/generated/canvas", true});
    result.reports["canvas"] = canvasReport(canvas);
    for (const auto &diagnostic : canvas.diagnostics) {
        auto noted = diagnostic;
        noted["lowering"] = "canvas";
        result.diagnostics.push_back(std::move(noted));
    }
    for (auto &source : canvas.generated_sources)
        result.generated_sources.push_back({std::move(source.path), std::move(source.text)});
    result.changed = canvas.changed;
    result.document = std::move(canvas.document);
#else
    result.document = graph_document;
#endif
    return result;
}

void writeAuthoringGeneratedSources(const AuthoringLowering &lowering, const std::filesystem::path &project_root) {
    for (const auto &source : lowering.generated_sources) {
        const auto path = project_root / std::filesystem::path{source.path};
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error) && readText(path) == source.text) continue;
        std::filesystem::create_directories(path.parent_path());
        auto partial = path;
        partial += ".partial";
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            out << source.text;
            if (!out) throw std::runtime_error("cannot write " + partial.string());
        }
        std::filesystem::rename(partial, path);
    }
}

} // namespace Pelican
