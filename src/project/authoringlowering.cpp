#include "authoringlowering.hpp"

#if PELICAN_WITH_CANVAS
#include "canvas/canvaslowering.hpp"
#endif
#include "renderingjsonnormalize.hpp"
#if PELICAN_WITH_VIDEO_OUTPUT
#include "output/outputlowering.hpp"
#endif

#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <tuple>

namespace Pelican {
namespace {
using Json = nlohmann::json;

#if PELICAN_WITH_CANVAS
// Each output: its items' kinds, which authored nodes drew each instance, and
// the files it generated: a vertex and a fragment source per draw (CV-2a: its
// groups' draws first, its own last), CV-4: a glyph atlas per font first.
Json canvasReport(const CanvasLowering &lowering) {
    auto outputs = Json::array();
    for (const auto &output : lowering.ir.outputs) {
        std::set<std::uint32_t> kinds;
        for (const auto &item : output.items) kinds.insert(canvasItemKind(item));
        auto kind_names = Json::array();
        for (const auto kind : kinds) kind_names.push_back(std::string{canvasItemKindName(kind)});
        auto items = Json::array();
        for (const auto &item : lowering.items)
            if (item.at("output") == output.id && item.value("scope", std::string{}) == output.scope)
                items.push_back({{"instance", item.at("instance")}, {"kind", item.at("kind")},
                                 {"nodes", item.at("nodes")}});
        auto generated = Json::array();
        for (const auto &source : lowering.generated_sources) {
            if (source.output != output.id || source.scope != output.scope) continue;
            Json entry{{"stem", source.stem}, {"stage", source.stage}, {"path", source.path}};
            if (source.stage == "atlas") entry["bytes"] = source.bytes.size();
            else entry["text"] = source.text;
            generated.push_back(std::move(entry));
        }
        Json row{{"id", output.id}};
        if (!output.scope.empty()) row["scope"] = output.scope;
        if (!output.groups.empty()) row["groups"] = output.groups.size();
        row["kinds"] = std::move(kind_names);
        row["items"] = std::move(items);
        row["generated"] = std::move(generated);
        if (!output.atlases.empty()) {
            auto atlases = Json::array();
            for (const auto &atlas : output.atlases)
                atlases.push_back({{"font", atlas.font}, {"face", atlas.face}, {"cell", atlas.cell},
                                   {"width", atlas.pixels->width}, {"height", atlas.pixels->height},
                                   {"path", atlas.path}});
            row["atlases"] = std::move(atlases);
        }
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

// The bytes a generated source holds, as a file would.
std::string sourceBytes(const AuthoringGeneratedSource &source) {
    if (source.bytes.empty()) return source.text;
    return {reinterpret_cast<const char *>(source.bytes.data()), source.bytes.size()};
}

// projectFileLoweringContext's memo: a file's bytes while its size and
// modification time stay the same.
struct FileMemo {
    std::mutex mutex;
    std::map<std::filesystem::path,
             std::tuple<std::uintmax_t, std::filesystem::file_time_type, std::shared_ptr<const std::vector<std::byte>>>>
        files;
};

FileMemo &fileMemo() {
    static FileMemo instance;
    return instance;
}

// Bounded: past it the memo starts over (only a reread is lost).
constexpr std::size_t file_memo_limit = 32;
} // namespace

AuthoringLoweringContext projectFileLoweringContext(
    std::function<std::optional<std::filesystem::path>(std::string_view reference)> resolve) {
    return {[resolve = std::move(resolve)](std::string_view reference) -> std::shared_ptr<const std::vector<std::byte>> {
        const auto path = resolve(reference);
        if (!path) return nullptr;
        std::error_code error;
        const auto size = std::filesystem::file_size(*path, error);
        if (error) return nullptr;
        const auto time = std::filesystem::last_write_time(*path, error);
        if (error) return nullptr;
        auto &memo = fileMemo();
        {
            const std::lock_guard lock{memo.mutex};
            if (const auto found = memo.files.find(*path);
                found != memo.files.end() && std::get<0>(found->second) == size && std::get<1>(found->second) == time)
                return std::get<2>(found->second);
        }
        std::ifstream in(*path, std::ios::binary);
        if (!in) return nullptr;
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        auto bytes = std::make_shared<const std::vector<std::byte>>(reinterpret_cast<const std::byte *>(text.data()),
                                                                    reinterpret_cast<const std::byte *>(text.data()) +
                                                                        text.size());
        const std::lock_guard lock{memo.mutex};
        if (memo.files.size() >= file_memo_limit) memo.files.clear();
        memo.files[*path] = {size, time, bytes};
        return bytes;
    }};
}

AuthoringLowering lowerAuthoringDocument(const nlohmann::json &authored_document,
                                         const AuthoringLoweringContext &context) {
    AuthoringLowering result;
    // RC-3b: the root's passes in the normal form before any lowering reads
    // them (canvas's rendering inputs here, the graph's in the composition).
    const auto graph_document = normalizeRenderingJson(authored_document);
#if PELICAN_WITH_CANVAS
    // pelican.canvas: one directory per shader pair (and per glyph atlas)
    // under the project's own metadata, so a pair's compile cache key never
    // moves with another's.
    auto canvas = lowerCanvasNodes(
        graph_document, CanvasLoweringOptions{".pelican/generated/canvas", true, ".pelican/generated/canvas"},
        context.read_asset);
    result.reports["canvas"] = canvasReport(canvas);
    for (const auto &diagnostic : canvas.diagnostics) {
        auto noted = diagnostic;
        noted["lowering"] = "canvas";
        result.diagnostics.push_back(std::move(noted));
    }
    for (auto &source : canvas.generated_sources)
        result.generated_sources.push_back({std::move(source.path), std::move(source.text), std::move(source.bytes)});
    result.changed = canvas.changed;
    result.document = std::move(canvas.document);
#else
    (void)context;
    result.document = graph_document;
#endif
#if PELICAN_WITH_VIDEO_OUTPUT
    // VO-1a: after canvas, because a canvas node may be the pass whose output
    // a sink names. The root document is still the lowered one; the authored
    // argument is const and stays untouched.
    auto output = lowerOutputNodes(result.document);
    result.reports["output"] = std::move(output.report);
    result.output_sinks = std::move(output.sinks);
    result.changed = result.changed || output.changed;
    result.document = std::move(output.document);
#endif
    return result;
}

void writeAuthoringGeneratedSources(const AuthoringLowering &lowering, const std::filesystem::path &project_root) {
    for (const auto &source : lowering.generated_sources) {
        const auto path = project_root / std::filesystem::path{source.path};
        const auto bytes = sourceBytes(source);
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error) && readText(path) == bytes) continue;
        std::filesystem::create_directories(path.parent_path());
        auto partial = path;
        partial += ".partial";
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            out << bytes;
            if (!out) throw std::runtime_error("cannot write " + partial.string());
        }
        std::filesystem::rename(partial, path);
    }
}

} // namespace Pelican
