#include "canvaslowercommand.hpp"

#include "../core/build_features.hpp"
#include "../project/graphcomposition.hpp"
#if PELICAN_WITH_CANVAS
#include "../project/canvas/canvaslowering.hpp"
#include "../project/canvas/canvasship.hpp"
#include "../project/projecttemplate.hpp"
#include "processrunner.hpp"
#endif

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican::DevCli {
namespace {
namespace fs = std::filesystem;
using OrderedJson = nlohmann::ordered_json;

#if PELICAN_WITH_CANVAS
constexpr std::string_view project_scheme = "project://";

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

// CV-4: a shape.text font, read from the project being lowered.
std::shared_ptr<const std::vector<std::byte>> readProjectAsset(const fs::path &project_dir, std::string_view reference) {
    if (!reference.starts_with(project_scheme)) return nullptr;
    const auto path = project_dir / fs::path{std::string{reference.substr(project_scheme.size())}};
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return nullptr;
    const auto text = readText(path);
    const auto *begin = reinterpret_cast<const std::byte *>(text.data());
    return std::make_shared<const std::vector<std::byte>>(begin, begin + text.size());
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

std::string generatedBytes(const CanvasGeneratedSource &generated) {
    return generated.bytes.empty()
               ? generated.text
               : std::string{reinterpret_cast<const char *>(generated.bytes.data()), generated.bytes.size()};
}

// What a child said went wrong, for a refusal: its error lines (the log's
// LOG_ERROR and LOG_CRITICAL, a named error), else the end of its output.
std::string failureText(const std::string &text) {
    constexpr std::size_t keep = 2000;
    std::string errors;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);)
        if (line.find("LOG_ERROR") != std::string::npos || line.find("LOG_CRITICAL") != std::string::npos ||
            line.find("@1") != std::string::npos)
            errors += line + "\n";
    const auto &chosen = errors.empty() ? text : errors;
    return chosen.size() <= keep ? chosen : chosen.substr(chosen.size() - keep);
}

// CV-5: draws each static run's document with the player, in a blank
// project of its own under the temporary directory, and keeps the frame it
// writes (the premultiplied linear canvas, sRGB-encoded, alpha kept) by the
// run's digest. The player takes no GPU lease of its own: whoever runs
// canvas-lower holds it (the GPU tests run their whole script under
// pelican_gpu_lease_run, which does not nest).
class RunPainter {
  public:
    RunPainter(fs::path project_dir, fs::path player, CanvasAssetReader read_asset)
        : project_dir_(std::move(project_dir)), player_(std::move(player)), read_asset_(std::move(read_asset)) {}
    ~RunPainter() {
        if (temporary_.empty()) return;
        std::error_code ignored;
        fs::remove_all(temporary_, ignored);
    }
    RunPainter(const RunPainter &) = delete;
    RunPainter &operator=(const RunPainter &) = delete;

    const std::string &paint(const CanvasFlattenRun &run) {
        if (const auto found = frames_.find(run.digest); found != frames_.end()) return found->second;
        const auto refuse = [&](const std::string &reason) {
            return GraphRegistryError("canvas.flatten_failed@1",
                                      {{"stack", run.stack}, {"layers", run.layers}, {"reason", reason}});
        };
        if (temporary_.empty()) {
            const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
            for (unsigned attempt = 0;; ++attempt) {
                const auto candidate = fs::temp_directory_path() / ("pelican_canvas_bake_" + std::to_string(tick) +
                                                                    "_" + std::to_string(attempt));
                if (fs::create_directory(candidate)) {
                    temporary_ = candidate;
                    break;
                }
            }
            initializeProjectTemplate(temporary_ / "project", ProjectTemplate::Blank);
        }
        const auto project = temporary_ / "project";
        // The images the run reads, at the same place in the run's project
        // (its fonts are read here, into the atlas the lowering writes).
        for (const auto &asset : run.assets) {
            if (!std::string_view{asset}.starts_with(project_scheme)) continue;
            const auto relative = fs::path{asset.substr(project_scheme.size())};
            std::error_code error;
            if (!fs::is_regular_file(project_dir_ / relative, error)) continue;
            fs::create_directories((project / relative).parent_path());
            fs::copy_file(project_dir_ / relative, project / relative, fs::copy_options::overwrite_existing);
        }
        const auto lowering = lowerCanvasNodes(run.document, {}, read_asset_);
        writeText(project / "passes" / "main_rendering_config.json", lowering.document.dump(2) + "\n");
        for (const auto &generated : lowering.generated_sources)
            writeText(project / generated.path, generatedBytes(generated));
        const auto frames = temporary_ / ("frames_" + run.digest);
        fs::create_directories(frames);
        ProcessOptions options;
        options.executable = player_;
        options.arguments = {"--headless", "--project", project, "--size",
                             std::to_string(run.pixels[0]) + "x" + std::to_string(run.pixels[1]),
                             "--fps", "60", "--frames", "2", "--render-out", frames / "%04d.png"};
        options.timeout = std::chrono::milliseconds{600000};
        options.name = "pelican_player (canvas flatten)";
        const auto result = runProcess(options);
        if (result.timed_out) throw refuse("the player did not finish within 600 s");
        if (result.exit_code != 0)
            throw refuse("the player exited with " + std::to_string(result.exit_code) + ": " +
                         failureText(result.stdout_text + "\n" + result.stderr_text));
        // The second frame, as the existing headless checks take a settled one.
        const auto frame = frames / "0002.png";
        std::error_code error;
        if (!fs::is_regular_file(frame, error)) throw refuse("the player wrote no " + frame.filename().string());
        auto png = readText(frame);
        const auto big_endian = [&](std::size_t at) {
            return (static_cast<std::uint32_t>(static_cast<unsigned char>(png[at])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(png[at + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(png[at + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(png[at + 3]));
        };
        if (png.size() < 24 || png.compare(1, 3, "PNG") != 0 || png.compare(12, 4, "IHDR") != 0)
            throw refuse("the player's frame is not a PNG");
        if (big_endian(16) != run.pixels[0] || big_endian(20) != run.pixels[1])
            throw refuse("the player's frame is " + std::to_string(big_endian(16)) + "x" +
                         std::to_string(big_endian(20)) + ", not " + std::to_string(run.pixels[0]) + "x" +
                         std::to_string(run.pixels[1]));
        return frames_.emplace(run.digest, std::move(png)).first->second;
    }

  private:
    fs::path project_dir_;
    fs::path player_;
    CanvasAssetReader read_asset_;
    fs::path temporary_;
    std::map<std::string, std::string> frames_;
};

// CV-5, ship: removes the files canvas nodes read (images, fonts) that no
// JSON file of the lowered copy names any more -- the layers folded into
// images, the fonts baked into atlases -- and the directories that leaves
// empty. Returns their references.
std::vector<std::string> removeUnreferenced(const fs::path &out_dir, const std::set<std::string> &candidates) {
    std::string named;
    for (const auto &entry : fs::recursive_directory_iterator(out_dir))
        if (entry.is_regular_file() && entry.path().extension() == ".json") named += readText(entry.path()) + "\n";
    std::vector<std::string> removed;
    const auto root = fs::weakly_canonical(out_dir);
    for (const auto &reference : candidates) {
        const auto relative = reference.substr(project_scheme.size());
        if (relative.empty() || named.find(relative) != std::string::npos) continue;
        auto path = fs::weakly_canonical(out_dir / fs::path{relative});
        const auto inside = path.lexically_relative(root);
        if (inside.empty() || *inside.begin() == "..") continue;
        std::error_code error;
        if (!fs::is_regular_file(path, error)) continue;
        fs::remove(path);
        removed.push_back(reference);
        for (path = path.parent_path(); path != root && fs::is_directory(path, error) && fs::is_empty(path, error);
             path = path.parent_path())
            fs::remove(path, error);
    }
    return removed;
}

int lower(const fs::path &project_dir, const fs::path &out_dir, CanvasLoweringPolicy policy,
          const std::optional<fs::path> &player) {
    const bool ship = policy == CanvasLoweringPolicy::ship;
    const auto project_file = project_dir / "project.json";
    const auto project_text = readText(project_file);
    auto project = OrderedJson::parse(project_text);
    const auto source = fs::weakly_canonical(project_dir);
    const auto target = fs::weakly_canonical(out_dir);
    const auto relative = target.lexically_relative(source);
    if (target == source || (!relative.empty() && *relative.begin() != ".."))
        throw std::runtime_error("canvas-lower --out must lie outside the project: " + out_dir.string());
    const CanvasAssetReader read_asset = [&](std::string_view reference) {
        return readProjectAsset(project_dir, reference);
    };

    // Lower every document before writing anything, so a refusal leaves no
    // half-written copy behind.
    struct Lowered {
        std::string path;
        std::string text;
        CanvasLowering lowering;
        // CV-5, ship: the static runs folded into images, and the
        // diagnostics of the folding, before the lowering's own.
        std::vector<CanvasFlattenRun> runs;
        OrderedJson diagnostics = OrderedJson::array();
    };
    std::vector<Lowered> lowered;
    std::optional<RunPainter> painter;
    if (ship && player) painter.emplace(project_dir, *player, read_asset);
    std::set<std::string> canvas_files;
    for (const auto &document : renderingDocuments(project)) {
        auto text = readText(project_dir / document);
        const auto authored = nlohmann::json::parse(text);
        Lowered entry{document, std::move(text), {}, {}, OrderedJson::array()};
        if (!ship) {
            entry.lowering = lowerCanvasNodes(authored, {}, read_asset);
        } else {
            for (const auto &asset : canvasDocumentAssets(authored))
                if (std::string_view{asset}.starts_with(project_scheme)) canvas_files.insert(asset);
            const auto plan = planCanvasFlatten(authored, read_asset);
            for (const auto &diagnostic : plan.diagnostics) entry.diagnostics.push_back(diagnostic);
            auto folded = authored;
            if (!plan.runs.empty() && !painter) {
                entry.diagnostics.push_back({{"code", "canvas.flatten_skipped"},
                                             {"reason", "no_player"},
                                             {"document", document},
                                             {"runs", plan.runs.size()}});
            } else if (!plan.runs.empty()) {
                for (const auto &run : plan.runs) (void)painter->paint(run);
                folded = applyCanvasFlatten(authored, plan);
                entry.runs = plan.runs;
            }
            CanvasLoweringOptions options;
            options.policy = CanvasLoweringPolicy::ship;
            entry.lowering = lowerCanvasNodes(folded, options, read_asset);
        }
        lowered.push_back(std::move(entry));
    }

    copyProject(project_dir, out_dir);
    for (const auto &entry : lowered) {
        // A document without a canvas op is copied byte for byte.
        if (!entry.lowering.changed) continue;
        writeText(out_dir / entry.path, entry.lowering.document.dump(2) + "\n");
        for (const auto &generated : entry.lowering.generated_sources)
            writeText(out_dir / generated.path, generatedBytes(generated));
        for (const auto &run : entry.runs) writeText(out_dir / canvasFlattenImagePath(run), painter->paint(run));
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
    const auto removed = ship ? removeUnreferenced(out_dir, canvas_files) : std::vector<std::string>{};

    OrderedJson report{{"policy", ship ? "ship" : "edit"},
                       {"outputs", OrderedJson::array()},
                       {"diagnostics", OrderedJson::array()}};
    auto flattened = OrderedJson::array();
    std::size_t folded_constants = 0;
    for (const auto &entry : lowered) {
        const auto &lowering = entry.lowering;
        for (std::size_t index = 0; index < lowering.ir.outputs.size(); ++index) {
            const auto &output = lowering.ir.outputs[index];
            std::set<std::uint32_t> kinds;
            for (const auto &item : output.items) kinds.insert(canvasItemKind(item));
            auto kind_names = OrderedJson::array();
            for (const auto kind : kinds) kind_names.push_back(std::string{canvasItemKindName(kind)});
            // Each draw of the output (its groups' first, its own last)
            // generated one vertex and one fragment source; CV-4: each font
            // face a glyph atlas.
            auto generated = OrderedJson::array();
            auto atlases = OrderedJson::array();
            for (const auto &source : lowering.generated_sources) {
                if (source.output != output.id || source.scope != output.scope) continue;
                if (source.stage == "vert") generated.push_back(source.stem);
                else if (source.stage == "atlas") atlases.push_back(source.path);
            }
            // CV-5: every instance drawn for the output, its groups' included.
            std::size_t instances = 0;
            for (const auto &item : lowering.items)
                if (item.value("output", std::string{}) == output.id &&
                    item.value("scope", std::string{}) == output.scope)
                    ++instances;
            OrderedJson row{{"id", output.id}};
            if (!output.scope.empty()) row["scope"] = output.scope;
            row["document"] = entry.path;
            row["items"] = output.items.size();
            row["instances"] = instances;
            if (!output.groups.empty()) row["groups"] = output.groups.size();
            row["kinds"] = std::move(kind_names);
            row["generated"] = std::move(generated);
            if (!atlases.empty()) row["atlases"] = std::move(atlases);
            report["outputs"].push_back(std::move(row));
        }
        for (const auto &diagnostic : entry.diagnostics) report["diagnostics"].push_back(diagnostic);
        for (const auto &diagnostic : lowering.diagnostics) report["diagnostics"].push_back(diagnostic);
        for (const auto &run : entry.runs) {
            OrderedJson row{{"output", run.output}};
            if (!run.scope.empty()) row["scope"] = run.scope;
            if (run.group) row["group"] = run.stack;
            row["document"] = entry.path;
            row["layers"] = run.layers;
            row["image"] = canvasFlattenImagePath(run);
            row["pixels"] = {run.pixels[0], run.pixels[1]};
            row["reason"] = run.reason;
            flattened.push_back(std::move(row));
        }
        folded_constants += lowering.folded_constants;
    }
    if (ship) {
        report["flattened"] = std::move(flattened);
        report["folded_constants"] = folded_constants;
        report["removed_assets"] = removed;
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
    program.add_argument("--policy").default_value(std::string{"edit"}).help("edit (the default) or ship");
    program.add_argument("--player")
        .default_value(std::string{})
        .help("ship: the pelican_player that draws the static runs folded into images");
    try {
        program.parse_args(argc, argv);
        const auto policy_name = program.get<std::string>("--policy");
        if (policy_name != "edit" && policy_name != "ship")
            throw std::invalid_argument("canvas-lower --policy must be edit or ship, not " + policy_name);
        const auto policy = policy_name == "ship" ? CanvasLoweringPolicy::ship : CanvasLoweringPolicy::edit;
        std::optional<fs::path> player;
        if (const auto argument = program.get<std::string>("--player"); !argument.empty()) {
            if (policy != CanvasLoweringPolicy::ship)
                throw std::invalid_argument("canvas-lower --player draws the static runs of --policy ship");
            std::error_code error;
            if (!fs::is_regular_file(argument, error))
                throw std::invalid_argument("canvas-lower --player names no file: " + argument);
            player = fs::absolute(argument);
        }
        return lower(program.get<std::string>("--project"), program.get<std::string>("--out"), policy, player);
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
