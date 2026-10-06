#include "graphcanvasmodel.hpp"

#include "frameplanmodel.hpp"
#include "projectformat.hpp"
#include "projectpathresolver.hpp"
#include "rendereditcompletion.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

std::string diagnostic(const Json &response) {
    const auto &error = response.at("error");
    if (error.is_string()) return error.get<std::string>();
    // LP-0d: a graph variant (preview, XR) that refused the edit is named
    // first, whatever the code -- its graph refusal keeps the graph's code.
    // A draw-loop refusal's own `variant` is not one (LP-2).
    const auto variant = graphRefusalVariant(error);
    const auto prefix =
        variant.empty() ? std::string{} : "The " + variant + " graph refused it: ";
    // GL-1d, decision 4: a route no pass draws reads as the Frame Plan's
    // sentence -- which materials, and the fix.
    if (auto unserved = materialRouteUnservedNotice(error); !unserved.empty())
        return prefix + unserved;
    // LP-4c, decision 9: a loop's or a definition's refusal is a sentence
    // made from the fields the engine lays beside its code; a view_select
    // one likewise (LP-4a).
    if (auto loop = graphLoopRefusalDescription(error); !loop.empty()) return prefix + loop;
    if (auto view = viewSelectRefusalDescription(error); !view.empty()) return prefix + view;
    auto message = error.value("message", std::string{"The graph read failed."});
    // Q-10: a code Studio has a sentence for reads as that sentence, also when
    // it arrives inside the message of a broader refusal (a sound refused
    // while the render pipeline prepared the edit).
    const auto code_text = error.contains("code") && error.at("code").is_string()
                               ? error.at("code").get<std::string>()
                               : std::string{};
    // RTL-2s, decision 3: a ray tracing refusal says where, from its fields.
    if (auto ray = rayTracingRefusalDescription(error); !ray.empty()) return prefix + ray;
    if (auto known = knownRefusalDescription(code_text, message); !known.empty())
        return prefix + known;
    if (error.contains("code")) {
        const auto &code = error.at("code");
        message += " (" + (code.is_string() ? code.get<std::string>() : code.dump()) + ")";
    }
    return prefix + message;
}

std::string text(const Json &value, const char *key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string()) return {};
    return found->get<std::string>();
}

// The node ids of a definition's graph.input nodes, in declaration order. An
// instance's input ports are exactly those: the catalog cannot declare them,
// because they follow the definition rather than the operation.
std::vector<std::string> definitionInputs(const Json &definition) {
    std::vector<std::string> ports;
    const auto nodes = definition.find("nodes");
    if (nodes == definition.end() || !nodes->is_array()) return ports;
    for (const auto &node : *nodes)
        if (node.is_object() && text(node, "op") == "graph.input")
            ports.push_back(text(node, "id"));
    return ports;
}

// The output names of a definition, in the order its outputs object keeps.
std::vector<std::string> definitionOutputs(const Json &definition) {
    std::vector<std::string> ports;
    const auto outputs = definition.find("outputs");
    if (outputs == definition.end() || !outputs->is_object()) return ports;
    for (const auto &[name, endpoint] : outputs->items()) {
        (void)endpoint;
        ports.push_back(name);
    }
    return ports;
}

bool isLoop(const Json &node) {
    return node.is_object() && text(node, "op") == "graph.loop";
}

// Q-19: the render nodes whose pass draws with a fragment shader, which can
// read numbered inputs (LP-1d) and whose pass publishes its declarations.
bool isGraphicsPass(std::string_view op) {
    return op == "render.fullscreen" || op == "render.raster";
}

// UX-7, decision 1 (engine RS-14). A numbered input a fullscreen pass gains
// reads an image the size of what it draws -- a bloom's chain, a blur, a tone
// map, a G-buffer -- whose far edge is not its near one: it is clamped there.
// The engine's default stays repeat, so Studio writes it. A raster pass's
// input (a displacement a vertex shader reads) keeps the default.
void clampFullscreenInput(std::string_view op, Json &port) {
    if (op == "render.fullscreen") port["sampling"] = Json{{"address", "clamp_to_edge"}};
}

// LP-4c. The bodies of a loop's branches, in order; an empty string is a
// branch with `"body": null` (or none the table can read).
std::vector<std::string> loopBranchBodies(const Json &node) {
    std::vector<std::string> bodies;
    const auto parameters = node.find("parameters");
    if (parameters == node.end() || !parameters->is_object()) return bodies;
    const auto branches = parameters->find("branches");
    if (branches == parameters->end() || !branches->is_array()) return bodies;
    for (const auto &branch : *branches)
        bodies.push_back(branch.is_object() ? text(branch, "body") : std::string{});
    return bodies;
}

// Q-19: whether a branch of this loop runs `body`.
bool runsBody(const Json &loop, std::string_view body) {
    const auto bodies = loopBranchBodies(loop);
    return std::ranges::find(bodies, body) != bodies.end();
}

// The integers of a `when` / `where` list, or nothing when one of them is not
// a literal integer (an expression the table has no form for).
std::optional<std::vector<std::int64_t>> integerList(const Json &values) {
    std::vector<std::int64_t> result;
    if (!values.is_array()) return std::nullopt;
    for (const auto &value : values) {
        if (!value.is_number_integer()) return std::nullopt;
        result.push_back(value.get<std::int64_t>());
    }
    return result;
}

// LP-4e. The draw steps of a draw_loop body (design 4.1).
bool isDrawStep(std::string_view op) {
    return op == "draw.material" || op == "draw.variant" || op == "draw.override";
}

// UX-3, decision 1: the render nodes the palette seeds besides
// render.fullscreen and render.material_pass (addNodeOpsIn).
bool isPaletteSeededRender(std::string_view op) {
    return op == "render.target_ref" || op == "render.image" || op == "render.buffer" ||
           op == "render.attribute" || op == "render.compute" || op == "render.shadow_depth" ||
           op == "render.snapshot_copy";
}

bool isDrawItemsLoop(const Json &node) {
    if (!isLoop(node)) return false;
    const auto parameters = node.find("parameters");
    if (parameters == node.end() || !parameters->is_object()) return false;
    const auto over = parameters->find("over");
    return over != parameters->end() && over->is_object() && over->contains("draw_items");
}

std::optional<std::vector<std::string>> stringList(const Json &values) {
    std::vector<std::string> result;
    if (!values.is_array()) return std::nullopt;
    for (const auto &value : values) {
        if (!value.is_string()) return std::nullopt;
        result.push_back(value.get<std::string>());
    }
    return result;
}

// {"include": [...], "exclude": [...], "require_all": [...]}, or nothing for
// a shape the fields cannot show.
std::optional<GraphCanvasTagFilter> readTagFilter(const Json &value) {
    if (!value.is_object()) return std::nullopt;
    GraphCanvasTagFilter filter;
    for (const auto &[key, list] : value.items()) {
        auto *target = key == "include"       ? &filter.include
                       : key == "exclude"     ? &filter.exclude
                       : key == "require_all" ? &filter.require_all
                                              : nullptr;
        auto values = stringList(list);
        if (target == nullptr || !values) return std::nullopt;
        *target = std::move(*values);
    }
    return filter;
}

Json tagFilterJson(const GraphCanvasTagFilter &filter) {
    Json value = Json::object();
    if (!filter.include.empty()) value["include"] = filter.include;
    if (!filter.exclude.empty()) value["exclude"] = filter.exclude;
    if (!filter.require_all.empty()) value["require_all"] = filter.require_all;
    return value;
}

// A draw_loop `when`, or nothing when it holds a key (or an operand) the
// table has no form for -- an expression, say.
std::optional<GraphCanvasDrawWhen> readDrawWhen(const Json &when) {
    if (!when.is_object() || when.empty()) return std::nullopt;
    GraphCanvasDrawWhen read;
    for (const auto &[key, operand] : when.items()) {
        if ((key == "material" || key == "contract") && operand.is_string()) {
            (key == "material" ? read.material : read.contract) = operand.get<std::string>();
        } else if (key == "tags") {
            auto tags = readTagFilter(operand);
            if (!tags) return std::nullopt;
            read.tags = std::move(*tags);
        } else if ((key == "skinned" || key == "has_variant") && operand.is_boolean()) {
            (key == "skinned" ? read.skinned : read.has_variant) = operand.get<bool>();
        } else if (key == "has_variant" && operand.is_string()) {
            // UX-3, decision 5: one variant by its name.
            read.has_variant_named = operand.get<std::string>();
        } else {
            return std::nullopt;
        }
    }
    return read;
}

Json drawWhenJson(const GraphCanvasDrawWhen &when) {
    Json value = Json::object();
    if (when.material) value["material"] = *when.material;
    if (when.tags) value["tags"] = tagFilterJson(*when.tags);
    if (when.contract) value["contract"] = *when.contract;
    if (when.skinned) value["skinned"] = *when.skinned;
    if (when.has_variant) value["has_variant"] = *when.has_variant;
    else if (when.has_variant_named) value["has_variant"] = *when.has_variant_named;
    return value;
}

// A branch's condition, read into the table's terms.
GraphCanvasLoopBranch readLoopBranch(const Json &branch) {
    GraphCanvasLoopBranch read;
    if (!branch.is_object()) {
        read.when = "other";
        return read;
    }
    const auto body = branch.find("body");
    if (body == branch.end() || body->is_null()) read.nothing = true;
    else if (body->is_string()) read.body = body->get<std::string>();
    const auto when = branch.find("when");
    if (when == branch.end()) return read;
    read.when = "other";
    // LP-4e: a draw_loop's condition names the material's attributes.
    if (auto draw = readDrawWhen(*when)) {
        read.when = "draw";
        read.draw_when = std::move(*draw);
        return read;
    }
    if (!when->is_object() || when->size() != 1) return read;
    const auto first = when->begin();
    const std::string key = first.key();
    const Json &value = first.value();
    if (key == "element" && value.is_number_integer()) {
        read.when = "element";
        read.values = {value.get<std::int64_t>()};
    } else if (key == "element_in" || key == "element_not_in") {
        if (auto values = integerList(value)) {
            read.when = key;
            read.values = std::move(*values);
        }
    }
    return read;
}

// A logical type and a logical type pattern both name the semantic type under
// the same key. That name is the whole of what the socket needs.
std::string semanticTypeName(const Json &owner, const char *key) {
    const auto found = owner.find(key);
    if (found == owner.end() || !found->is_object()) return {};
    return text(*found, "semantic");
}

// graph.input is structural: no module registers it, so the catalog cannot
// declare its type. Composition expands it into value.parameter, whose output
// is the scalar -- so that, and not "unknown", is what leaves its port.
constexpr auto kGraphInputType = "pelican.value.scalar@1";

// LP-4c: the one parameter-dependent operation the palette offers, seeded
// whole by addNodeOps.
constexpr std::string_view kFullscreenTemplate = "render.fullscreen";

// LP-4e: what a draw step hands on and a material pass's `draws` takes.
constexpr std::string_view kDrawStreamType = "pelican.render.draw_stream@1";
constexpr std::string_view kMaterialPass = "render.material_pass";

// GL-1f (engine RS-4): a pass that hands on the image it wrote as an output
// port of its input socket's name -- a material pass's colour sockets and
// depth, a shadow_depth's depth, a snapshot_copy's destination. The input a
// port hands on, or empty when the node forwards no such port.
std::string forwardedInputOf(const Json &node, std::string_view port) {
    if (!node.is_object() || port.empty()) return {};
    const auto op = text(node, "op");
    if (op == kMaterialPass) {
        if (port == "depth") return "depth";
        const auto sockets =
            GraphCanvasModel::materialPassColorSockets(node.value("parameters", Json::object()));
        return std::ranges::find(sockets, port) != sockets.end() ? std::string{port}
                                                                 : std::string{};
    }
    if (op == "render.shadow_depth" && port == "depth") return "depth";
    if (op == "render.snapshot_copy" && port == "destination") return "destination";
    return {};
}

// Whether the node's authored `outputs` array lists the port.
bool listsOutput(const Json &node, std::string_view port) {
    const auto outputs = node.find("outputs");
    return outputs != node.end() && outputs->is_array() &&
           std::ranges::any_of(*outputs, [&](const Json &output) {
               return output.is_string() && output.get<std::string>() == port;
           });
}

void collectUiNodes(const Json &node, const std::string &parent,
                    std::vector<GraphCanvasUiNode> &out) {
    if (!node.is_object()) return;
    const auto id = text(node, "id");
    if (id.empty()) return;
    auto path = parent.empty() ? id : parent + "/" + id;
    out.push_back(GraphCanvasUiNode{path, text(node, "type")});
    const auto children = node.find("children");
    if (children == node.end() || !children->is_array()) return;
    for (const auto &child : *children) collectUiNodes(child, path, out);
}

// Q-19. Whether a pass's input sockets are drawn from its shader: the engine
// published the declarations and there is at least one to draw (the Frame
// Plan's framePlanNodeDeclaresInputs, restated -- the view module has it).
bool declaresPassInputs(const FramePlanPassInputs &inputs) {
    return inputs.declared() && (!inputs.declarations.empty() || inputs.variadic_range);
}

// The image type a new render.fullscreen writes (addNodeOps) and the one a
// numbered port is given when nothing says what its source carries.
Json sceneColorType() {
    return Json{{"semantic", "pelican.render.color_signal@1"},
                {"arguments", Json{{"reference", "scene"}, {"transfer", "linear"},
                                   {"range", "extended"}}}};
}

// UX-8, decision 2 (UX-13a, decision 1): the named buffer port a buffer
// declaration of the shader reads. A declaration named pelican_resource_<port>
// is that port's, wherever it sits: the engine binds a port to the variable
// of its name (RS-5), and to nothing else. Any other declaration is the one
// at the binding the engine gives a buffer read: the pass's buffer reads in
// the order of the buffers they read (by resource name -- the graph's
// lowering keeps a pass's reads in a set), after the images, so read i sits
// at buffer_binding_base + i (passBufferInputBinding). `reads` are the
// pass's buffer ports in that order (GraphCanvasModel::bufferPortReads).
std::string bufferSocketPort(const FramePlanPassInputs &declared,
                             const std::vector<GraphCanvasModel::BufferPortRead> &reads,
                             const FramePlanPassInputDeclaration &declaration) {
    if (reads.empty()) return {};
    if (declaration.name.starts_with(GraphCanvasModel::kResourcePortVariablePrefix)) {
        const auto named = GraphCanvasModel::bufferPortName(declaration.name);
        return std::ranges::any_of(reads, [&](const auto &read) { return read.port == named; })
                   ? named
                   : std::string{};
    }
    if (declaration.binding < declared.buffer_binding_base) return {};
    // One binding per buffer read, however many ports read it.
    std::vector<std::string> resources;
    for (const auto &read : reads)
        if (!read.resource.empty() && std::ranges::find(resources, read.resource) == resources.end())
            resources.push_back(read.resource);
    const auto place = declaration.binding - declared.buffer_binding_base;
    if (place >= resources.size()) return {};
    const auto found = std::ranges::find(reads, resources[place], &GraphCanvasModel::BufferPortRead::resource);
    return found != reads.end() ? found->port : std::string{};
}

// Q-19, decision 1: a render node's sockets, from its pass's declarations and
// its numbered ports -- VI-3a's framePlanInputSockets, with a numbered port
// where the Frame Plan has a pass's input. Binding n is input n: the numbered
// inputs come first in the pass's `input` (LP-1d).
std::vector<GraphCanvasInputSocket> graphInputSockets(
    const FramePlanPassInputs &declared, const Json &node,
    const std::vector<GraphCanvasModel::BufferPortRead> &buffer_reads) {
    using Kind = GraphCanvasInputSocket::Kind;
    std::vector<GraphCanvasInputSocket> sockets;
    const auto parameters = node.value("parameters", Json::object());
    const auto numbered =
        GraphCanvasModel::numberedPorts(parameters.is_object() ? parameters : Json::object());
    const auto inputs = node.value("inputs", Json::object());
    std::set<std::int64_t> taken;
    const auto connect = [&](GraphCanvasInputSocket &socket, std::int64_t index) {
        const auto found = numbered.find(index);
        if (found == numbered.end()) return;
        socket.port = found->second;
        if (inputs.is_object())
            if (const auto edge = inputs.find(socket.port); edge != inputs.end() && edge->is_object()) {
                socket.source_node = text(*edge, "node");
                socket.source_port = text(*edge, "port");
            }
    };
    const auto source = [](const GraphCanvasInputSocket &socket) {
        return socket.source_node.empty() ? socket.port
                                          : socket.source_node + "." + socket.source_port;
    };
    bool variadic_drawn = false;
    for (const auto &declaration : declared.declarations) {
        if (declaration.variadic) {
            if (variadic_drawn || !declared.variadic_range) continue;
            variadic_drawn = true;
            const auto [first, max] = *declared.variadic_range;
            for (std::size_t slot = 0; slot < max; ++slot) {
                const auto ordinal = static_cast<std::int64_t>(first + slot);
                taken.insert(ordinal);
                if (!numbered.contains(ordinal)) continue;
                GraphCanvasInputSocket socket;
                socket.kind = Kind::variadic;
                socket.binding = first + slot;
                connect(socket, ordinal);
                socket.label = "← " + source(socket);
                sockets.push_back(std::move(socket));
            }
            GraphCanvasInputSocket drop;
            drop.kind = Kind::variadic_drop;
            drop.binding = std::max(first, numbered.size());
            drop.label = "+ input";
            sockets.push_back(std::move(drop));
            continue;
        }
        GraphCanvasInputSocket socket;
        socket.kind = Kind::declared;
        socket.binding = declaration.binding;
        socket.declaration = declaration.name;
        socket.buffer = declaration.buffer();
        if (!socket.buffer) {
            const auto index = static_cast<std::int64_t>(declaration.binding);
            taken.insert(index);
            connect(socket, index);
        } else if (const auto port = bufferSocketPort(declared, buffer_reads, declaration); !port.empty()) {
            // UX-8, decision 2: the named buffer port that binding reads.
            socket.port = port;
            if (inputs.is_object())
                if (const auto edge = inputs.find(port); edge != inputs.end() && edge->is_object()) {
                    socket.source_node = text(*edge, "node");
                    socket.source_port = text(*edge, "port");
                }
        }
        socket.label = socket.connected() ? declaration.name + " ← " + source(socket)
                                          : declaration.name;
        sockets.push_back(std::move(socket));
    }
    // A numbered port no declaration takes keeps a socket of its own, named
    // by the port, so its line still has an end.
    for (const auto &[index, port] : numbered) {
        if (taken.contains(index)) continue;
        GraphCanvasInputSocket socket;
        socket.kind = Kind::undeclared;
        socket.binding = static_cast<std::size_t>(index);
        connect(socket, index);
        socket.label = port + " ← " + source(socket);
        sockets.push_back(std::move(socket));
    }
    return sockets;
}

// "<from>" or "<from>..<last>", the bindings a skipped drop leaves empty.
std::string bindingRange(std::size_t from, std::size_t to_exclusive) {
    return to_exclusive - from == 1
               ? std::to_string(from)
               : std::to_string(from) + ".." + std::to_string(to_exclusive - 1);
}
} // namespace

std::vector<GraphCanvasUiNode> graphCanvasUiDocumentNodes(const Json &document) {
    std::vector<GraphCanvasUiNode> nodes;
    if (!document.is_object()) return nodes;
    const auto root = document.find("root");
    if (root != document.end()) collectUiNodes(*root, {}, nodes);
    return nodes;
}

GraphCanvasUiNodesRead readProjectUiNodes(const std::filesystem::path &project_root) {
    GraphCanvasUiNodesRead read;
    try {
        std::ifstream input{project_root / "project.json", std::ios::binary};
        if (!input) {
            read.error = "project.json could not be opened, so the UI document is unknown";
            return read;
        }
        const std::string contents{std::istreambuf_iterator<char>{input},
                                   std::istreambuf_iterator<char>{}};
        const auto parsed = Pelican::parseProjectEnvelopeText(
            contents, Pelican::ProjectEnvelopeParseOptions{.ignore_engine_version = true});
        const auto &basic = parsed.envelope.basic_config;
        const auto reference = basic.find("ui_config_json");
        if (reference == basic.end() || !reference->is_string() ||
            reference->get_ref<const std::string &>().empty()) {
            read.error = "project.json names no UI document (basic_config.ui_config_json)";
            return read;
        }
        Pelican::ProjectPathResolver resolver;
        resolver.setup(project_root, false, parsed.envelope);
        read.nodes = graphCanvasUiDocumentNodes(
            Json::parse(resolver.loadText(reference->get<std::string>())));
        if (read.nodes.empty())
            read.error = "the UI document " + reference->get<std::string>() +
                         " has no node with an id";
    } catch (const std::exception &error) {
        read.nodes.clear();
        read.error = std::string{"the UI document could not be read: "} + error.what();
    }
    return read;
}

GraphCanvasAudioAssetsRead readProjectAudioAssets(const std::filesystem::path &project_root) {
    GraphCanvasAudioAssetsRead read;
    if (project_root.empty()) {
        read.error = "no project is open";
        return read;
    }
    std::error_code error;
    std::filesystem::recursive_directory_iterator walk{
        project_root, std::filesystem::directory_options::skip_permission_denied, error};
    if (error) {
        read.error = "the project folder could not be read: " + error.message();
        return read;
    }
    try {
        for (auto end = std::filesystem::recursive_directory_iterator{}; walk != end;
             walk.increment(error)) {
            if (error) break;
            const auto &entry = *walk;
            std::error_code kind;
            if (entry.is_directory(kind)) {
                const auto name = entry.path().filename().u8string();
                if (!name.empty() && name.front() == u8'.') walk.disable_recursion_pending();
                continue;
            }
            auto extension = entry.path().extension().u8string();
            std::ranges::transform(extension, extension.begin(), [](char8_t letter) {
                return letter < 0x80 ? static_cast<char8_t>(std::tolower(letter)) : letter;
            });
            if (extension != u8".wav" && extension != u8".ogg") continue;
            const auto relative = entry.path().lexically_relative(project_root).generic_u8string();
            if (relative.empty() || relative.starts_with(u8"..")) continue;
            read.assets.push_back("project://" +
                                  std::string{relative.begin(), relative.end()});
        }
    } catch (const std::exception &caught) {
        read.error = std::string{"the project folder could not be read: "} + caught.what();
    }
    std::ranges::sort(read.assets);
    if (read.assets.empty() && read.error.empty())
        read.error = "no .wav or .ogg file in the project";
    return read;
}

bool GraphCanvasModel::isAudioPlayback(std::string_view op) noexcept {
    return op == "audio.playback";
}

const std::vector<std::string> &GraphCanvasModel::audioBusChoices() {
    static const std::vector<std::string> buses{"se", "bgm"};
    return buses;
}

const std::vector<std::string> &GraphCanvasModel::audioWrapChoices() {
    static const std::vector<std::string> wraps{"once", "repeat", "ping_pong"};
    return wraps;
}

std::string_view GraphCanvasModel::uiNodeParameter(std::string_view op) noexcept {
    return op == "ui.value" || op == "ui.visible" ? std::string_view{"node"}
                                                  : std::string_view{};
}

std::vector<std::string> GraphCanvasModel::uiNodeChoices(std::string_view op) const {
    std::vector<std::string> choices;
    if (uiNodeParameter(op).empty()) return choices;
    // A value write to anything but a gauge is ui.property_mismatch@1; the
    // engine would refuse it by name, so the list does not offer it.
    const bool value = op == "ui.value";
    for (const auto &node : ui_nodes_)
        if (!value || node.type == "gauge") choices.push_back(node.path);
    return choices;
}

// GL-1, decision 3. A reply is always about the real document: while it is
// read, the folded view on screen gives the real one back, and afterwards is
// built again from its trail -- as far down the trail as the new document and
// plan still hold. A view that comes out the same keeps the scope entered in
// it, and draws nothing again.
struct GraphCanvasModel::FoldedSuspend {
    GraphCanvasModel &model;
    std::vector<GraphCanvasFoldedStep> trail;
    std::vector<std::string> inner;
    std::string entered_loop;
    Json nodes;
    Json definitions;

    explicit FoldedSuspend(GraphCanvasModel &owner) : model{owner} {
        if (!model.folded_stash_) return;
        trail = model.folded_trail_;
        inner = model.breadcrumb_;
        entered_loop = model.entered_loop_;
        nodes = model.nodes_;
        definitions = model.definitions_;
        model.restoreFoldedStash();
    }
    FoldedSuspend(const FoldedSuspend &) = delete;
    FoldedSuspend &operator=(const FoldedSuspend &) = delete;
    ~FoldedSuspend() {
        if (trail.empty() || model.folded_stash_) return;
        // Leaving and entering around a reply is not the user's doing:
        // whether an edit was refused in here stays as it was.
        const bool refused = model.folded_refused_;
        const auto content = model.content_revision_;
        model.folded_trail_ = trail;
        while (!model.folded_trail_.empty() && !model.showFoldedTop())
            model.folded_trail_.pop_back();
        if (model.folded_trail_.empty()) {
            // Nothing it showed is there any more: the real root it is.
            model.folded_refused_ = false;
            ++model.content_revision_;
            ++model.report_revision_;
            return;
        }
        model.folded_refused_ = refused;
        if (model.folded_trail_ != trail || model.nodes_ != nodes ||
            model.definitions_ != definitions)
            return;
        // The same view: the scope entered inside it stands, and the canvas
        // has nothing new to draw.
        if (std::ranges::all_of(inner.begin() + (inner.empty() ? 0 : 1), inner.end(),
                                [&](const std::string &scope) {
                                    return model.definitions_.contains(scope);
                                })) {
            model.breadcrumb_ = inner;
            model.entered_loop_ = entered_loop;
        }
        model.content_revision_ = content;
    }
};

void GraphCanvasModel::request(RequestKind kind, std::string method, Json params) {
    const auto id = next_request_++;
    pending_ = PendingRequest{id, kind};
    requests_.push_back({id, std::move(method), std::move(params)});
}

void GraphCanvasModel::read(RequestKind kind, std::string method, Json params) {
    const auto id = next_request_++;
    // One slot per kind. Asking again replaces the slot, so the older answer
    // is dropped when it arrives rather than overwriting the newer one.
    std::erase_if(reads_, [kind](const PendingRequest &pending) {
        return pending.kind == kind;
    });
    reads_.push_back(PendingRequest{id, kind});
    requests_.push_back({id, std::move(method), std::move(params)});
}

std::optional<GraphCanvasModel::PendingRequest>
GraphCanvasModel::takeRead(std::uint64_t id) {
    const auto found = std::ranges::find(reads_, id, &PendingRequest::id);
    if (found == reads_.end()) return std::nullopt;
    const auto pending = *found;
    reads_.erase(found);
    return pending;
}

void GraphCanvasModel::clearContext() {
    // OT-1, decision 3: the history outlives the document it was read with.
    // An engine that comes back (a restart after a graph edit asked for one)
    // serves the same document, and the digest the next read brings decides:
    // one other than history_digest_ clears it (see receiveRpcResult). Only
    // the edit in the air went with the engine. Another project clears it
    // through forgetHistory().
    discardPendingHistory();
    digest_.clear();
    layout_retry_.reset();
    layout_retrying_ = false;
    nodes_ = Json::array();
    definitions_ = Json::object();
    layout_ = Json::object();
    definitions_reported_ = false;
    context_received_ = false;
    plan_after_commit_ = false;
    breadcrumb_ = {std::string{}};
    entered_loop_.clear();
    loop_reports_ = Json::object();
    loop_reports_received_ = false;
    view_families_.clear();
    plan_node_views_.clear();
    plan_targets_.clear();
    plan_pass_inputs_.clear();
    plan_loop_copies_.clear();
    plan_passes_.clear();
    plan_graph_.clear();
    plan_retry_budget_ = 0;
    plan_retry_wanted_ = false;
    committed_renames_.clear();
    refusal_marks_.clear();
    parameter_refusal_.reset();
    values_received_ = false;
    values_published_ = false;
    values_ = Json::array();
    clearObservation();
    ++report_revision_;
    ++content_revision_;
}

void GraphCanvasModel::clearObservation() {
    node_values_instance_.clear();
    node_values_received_ = false;
    node_values_published_ = false;
    node_values_ = Json::array();
    // The question in the air was about what is no longer on screen. Letting
    // its slot stand would make the next ask look like a duplicate of it.
    std::erase_if(reads_, [](const PendingRequest &pending) {
        return pending.kind == RequestKind::NodeValues ||
               pending.kind == RequestKind::Status;
    });
}

void GraphCanvasModel::setWatchedInstance(std::string instance) {
    values_instance_ = std::move(instance);
    values_ = Json::array();
    values_received_ = false;
    values_published_ = false;
    clearObservation();
    clearTimeline();
    ++report_revision_;
}

void GraphCanvasModel::setConnected(bool connected) {
    if (connected_ == connected) return;
    // GL-1: a folded view is built from the document and the plan it is
    // about to lose.
    leaveFolded();
    connected_ = connected;
    templates_requested_ = false;
    templates_ = Json::object();
    pending_.reset();
    reads_.clear();
    scrubbing_ = false;
    status_received_ = false;
    status_time_ = 0;
    animation_available_ = false;
    animation_sinks_ = Json::array();
    clearTimeline();
    requests_.clear();
    catalog_requested_ = false;
    catalog_received_ = false;
    vocabulary_asked_ = false;
    // UX-5b: the engine's shader list is its binary's; one that arrived
    // stays, one that did not is asked again when a list is opened.
    engine_shaders_asked_ = false;
    engine_shaders_again_at_.reset();
    // EL-5s: a stem's interface is the project's files, which another engine
    // (another project) reads differently: asked again.
    shader_interfaces_.clear();
    shader_interface_reads_.clear();
    shader_interface_again_at_.clear();
    shader_interface_refreshing_.clear();
    pending_compute_shader_.reset();
    compute_port_changes_.clear();
    ++shader_interface_revision_;
    operations_ = Json::object();
    types_ = Json::object();
    failure_ = {};
    edit_notice_ = {};
    engine_restart_.reset();
    ticket_.clear();
    clearContext();
    refresh();
    requestCatalog();
}

void GraphCanvasModel::refresh() {
    if (canRefresh()) request(RequestKind::Context, "get_graph_instances", Json::object());
}

void GraphCanvasModel::engineRestartDeclined(std::string message) {
    // GL-1b: the engine that committed the edit is still the one running --
    // read what it has now (the plan comes with the document), as after any
    // other commit, and say why nothing restarted.
    engine_restart_.reset();
    edit_notice_ = {InspectorNoticeKind::Information, std::move(message)};
    refresh();
}

void GraphCanvasModel::requestCatalog() {
    if (!connected_ || catalog_requested_) return;
    catalog_requested_ = true;
    read(RequestKind::Catalog, "get_graph_catalog", Json::object());
}

void GraphCanvasModel::requestValues() {
    if (connected_ && !values_instance_.empty())
        read(RequestKind::Values, "get_graph_values", {{"instance", values_instance_}});
}

std::string GraphCanvasModel::nodeValuesInstance() const {
    // The root document is the empty question; a definition body is read
    // through the instance the "Values from" control names, because a
    // definition on its own has no generation that fed it.
    return currentScope().empty() ? std::string{} : values_instance_;
}

void GraphCanvasModel::requestObservation() {
    // The overlay is what the user asked for by naming an instance. Without
    // one, the canvas is the document's and nothing is read -- except, UX-8
    // decision 6, the root's own nodes while Live is on: the root is the
    // empty question, which needs no instance (a graph of root nodes only
    // has none to name).
    if (!connected_) return;
    if (values_instance_.empty() && !(live_ && currentScope().empty())) return;
    // A pair already in the air is the answer to the same question. Asking
    // again would only drop it: a Live tick on an engine slower than the tick
    // must not build a queue of reads nobody will ever look at.
    if (std::ranges::any_of(reads_, [](const PendingRequest &pending) {
            return pending.kind == RequestKind::NodeValues;
        }))
        return;
    const auto instance = nodeValuesInstance();
    // The status goes first, and the order is load-bearing: one client, one
    // pipe, replies in the order they were asked for. A Timeline sample is
    // taken when the node values land, and it is stamped with the time the
    // status just reported -- so the status has to be the earlier question or
    // every sample would carry the previous tick's clock.
    read(RequestKind::Status, "get_status", Json::object());
    read(RequestKind::NodeValues, "get_graph_node_values",
         instance.empty() ? Json::object() : Json{{"instance", instance}});
}

void GraphCanvasModel::requestLoopReports() {
    // GL-1, decision 1: the plan is read with every document, whatever the
    // root holds -- the passes no graph node made are drawn from it (LP-4c
    // read it only for a loop, a render node or an instance that draws).
    if (!connected_) {
        loop_reports_ = Json::object();
        loop_reports_received_ = false;
        view_families_.clear();
        plan_node_views_.clear();
        plan_targets_.clear();
        plan_pass_inputs_.clear();
        plan_loop_copies_.clear();
        plan_passes_.clear();
        plan_graph_.clear();
        plan_buffers_.clear();
        render_authoring_ = Json::object();
        ray_layout_read_.reset();
        ++authoring_revision_;
        plan_retry_wanted_ = false;
        return;
    }
    read(RequestKind::Loops, "get_frame_plan", Json::object());
}

void GraphCanvasModel::requestNextLoopPage() {
    if (!connected_ || !loop_page_node_.empty()) return;
    for (const auto &[node, report] : loop_reports_.items()) {
        if (loop_pages_done_.contains(node)) continue;
        if (text(report, "tier") != "draw_loop") continue;
        const auto held = report.value("elements", Json::array()).size();
        const auto total = report.contains("total") && report.at("total").is_number_unsigned()
                               ? report.at("total").get<std::size_t>()
                               : held;
        if (held >= total) {
            loop_pages_done_.insert(node);
            continue;
        }
        // The largest page the engine gives, from the first material not
        // held yet.
        loop_page_node_ = node;
        read(RequestKind::LoopPage, "get_frame_plan",
             Json{{"loop_elements", Json{{"node", node}, {"offset", held}, {"limit", 4096}}}});
        return;
    }
}

void GraphCanvasModel::receiveLoopPage(const Json &plan) {
    const auto node = std::exchange(loop_page_node_, {});
    const auto held = loop_reports_.find(node);
    const auto loops = plan.find("loops");
    bool added = false;
    if (held != loop_reports_.end() && loops != plan.end() && loops->is_array())
        for (const auto &loop : *loops) {
            if (!loop.is_object() || text(loop, "node") != node) continue;
            auto &elements = (*held)["elements"];
            if (!elements.is_array()) elements = Json::array();
            // The page is the one asked for only if it starts where the held
            // list ends; a plan that moved in between is not spliced in.
            if (loop.value("offset", std::size_t{0}) != elements.size()) break;
            for (const auto &element : loop.value("elements", Json::array())) {
                elements.push_back(element);
                added = true;
            }
            if (loop.contains("total")) (*held)["total"] = loop.at("total");
            break;
        }
    // A page that brought nothing ends that loop's reading.
    if (!added) loop_pages_done_.insert(node);
    requestNextLoopPage();
}

const Json *GraphCanvasModel::loopReport(std::string_view id) const {
    const auto found = loop_reports_.find(std::string{id});
    return found == loop_reports_.end() ? nullptr : &*found;
}

void GraphCanvasModel::watchInstance(std::string instance) {
    if (values_instance_ == instance) return;
    setWatchedInstance(std::move(instance));
    requestValues();
    requestObservation();
}

void GraphCanvasModel::setLive(bool live) {
    if (live_ == live) return;
    live_ = live;
    ++report_revision_;
    if (live_) requestObservation();
}

void GraphCanvasModel::setTimelineNode(std::string id) {
    if (timeline_node_ == id) return;
    timeline_node_ = std::move(id);
    clearTimeline();
    // No revision bump: which node the strip follows is the strip's business,
    // and nothing on the canvas is drawn differently for it. Bumping here
    // would redraw the canvas on every selection -- including the selection a
    // click on a pill makes, which would delete the editor that same click
    // just opened.
}

void GraphCanvasModel::clearTimeline() {
    timeline_.clear();
    timeline_ports_.clear();
}

bool GraphCanvasModel::scrub(double time) {
    if (!connected_ || scrubbing_) return false;
    if (!scrub_blocked_reason_.empty()) return false;
    if (!std::isfinite(time)) return false;
    scrubbing_ = true;
    // O-1: a scrub is the clock plus one drawn frame. The order is the whole
    // of the rule -- rendering before the time is set draws the old moment --
    // so the two go out as one pair, in it. Neither takes the edit slot: a
    // scrub is not a change to the document, and must not make the canvas
    // look busy to an edit that is trying to land.
    requests_.push_back({next_request_++, "set_time", Json{{"t", time}}});
    scrub_render_ = next_request_++;
    requests_.push_back({scrub_render_, "render_frame", Json::object()});
    return true;
}

void GraphCanvasModel::poll() {
    // UX-5d (EL-4c): a list the engine is still building is asked for again.
    if (engine_shaders_again_at_ && connected_ && !engine_shaders_known_ &&
        std::chrono::steady_clock::now() >= *engine_shaders_again_at_) {
        engine_shaders_again_at_.reset();
        read(RequestKind::EngineShaders, "get_engine_shaders", Json::object());
    }
    // EL-5s: a stem the engine is still reflecting is asked again; a Shader
    // picked again whose interface has arrived is sent.
    if (connected_ && !shader_interface_again_at_.empty()) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> due;
        for (const auto &[reference, at] : shader_interface_again_at_)
            if (now >= at) due.push_back(reference);
        for (const auto &reference : due) {
            shader_interface_again_at_.erase(reference);
            askShaderInterface(reference);
        }
    }
    if (pending_compute_shader_ && canEdit()) {
        const auto known = shaderInterface(pending_compute_shader_->reference);
        if (known && known->status != "building" &&
            !shaderInterfaceWaiting(pending_compute_shader_->reference)) {
            const auto pending = *std::exchange(pending_compute_shader_, std::nullopt);
            if (known->ready()) {
                if (auto ops = computeShaderOps(pending.scope, pending.node, pending.reference);
                    !ops.empty())
                    submitOps(std::move(ops));
            } else {
                edit_notice_ = {InspectorNoticeKind::Error,
                                "The engine could not read " + pending.reference + "'s ports: " +
                                    known->error};
                ++report_revision_;
            }
        }
    }
    if (!pollWanted()) return;
    requestCatalog();
    // Live is the only thing that reads on a clock. Off, the overlay follows
    // the document, which is what it did before the switch existed.
    if (live_) requestObservation();
    // Q-19: a render node whose pass the last plan did not list yet.
    if (std::exchange(plan_retry_wanted_, false) && plan_retry_budget_ > 0) {
        --plan_retry_budget_;
        read(RequestKind::Loops, "get_frame_plan", Json::object());
    } else if (once_follow_reads_ > 0) {
        // PS-1s, decision 4: the frame after a Recompute runs the passes; the
        // plan says when they last ran.
        if (--once_follow_reads_ == 0) once_follow_from_.clear();
        read(RequestKind::Loops, "get_frame_plan", Json::object());
    } else if (drawn_follow_reads_ > 0 &&
               std::ranges::none_of(reads_, [](const PendingRequest &pending) {
                   return pending.kind == RequestKind::Loops;
               })) {
        // UX-12c, decision 1: one read at a time, so an answer that takes
        // longer than a poll is not dropped for the next.
        --drawn_follow_reads_;
        read(RequestKind::Loops, "get_frame_plan", Json::object());
    }
    // An accepted edit owns the request slot until the engine says what became
    // of it. Nothing else may take that slot in the meantime.
    if (!pending_ && !ticket_.empty())
        request(RequestKind::Result, "get_edit_result", {{"ticket", ticket_}});
}

std::vector<InspectorRpcRequest> GraphCanvasModel::takeRpcRequests() {
    return std::exchange(requests_, {});
}

void GraphCanvasModel::receiveRpcFailure(std::uint64_t id, std::string message) {
    if (scrubbing_ && scrub_render_ == id) {
        scrubbing_ = false;
        scrub_render_ = 0;
        failure_ = {InspectorNoticeKind::Error, std::move(message)};
        ++report_revision_;
        return;
    }
    // EL-5s: get_shader_interface has slots of its own.
    if (takeShaderInterfaceReply(id, nullptr, message)) return;
    // GL-1: a folded view on screen stands aside while the reply is read.
    const FoldedSuspend suspend{*this};
    if (const auto pending = takeRead(id)) {
        const auto kind = pending->kind;
        // LP-4c: the loop report only adds marks to what the document
        // already draws. Without it the canvas says "loop" and nothing else.
        // GL-1: an engine without get_graph_templates has no decomposed
        // version to show, which is not an error.
        // GL-1e: nor is one without get_render_authoring_context: the
        // rendering section's passes then take no lines.
        // UX-5b: nor is one without get_engine_shaders: the lists stay the
        // fixed ones.
        if (kind == RequestKind::Loops || kind == RequestKind::Templates ||
            kind == RequestKind::Authoring || kind == RequestKind::EngineShaders ||
            kind == RequestKind::RayLayout)
            return;
        if (kind == RequestKind::LoopPage) {
            loop_pages_done_.insert(std::exchange(loop_page_node_, {}));
            requestNextLoopPage();
            return;
        }
        if (kind == RequestKind::RerunOnce) {
            receiveRerunOnceRefusal(message);
            return;
        }
        if (kind == RequestKind::Catalog) catalog_requested_ = false;
        // The canvas keeps standing: a report that did not arrive removes an
        // overlay, never the graph the document already described.
        failure_ = {InspectorNoticeKind::Error, std::move(message)};
        ++report_revision_;
        return;
    }
    if (!pending_ || pending_->id != id) return;
    const auto kind = pending_->kind;
    pending_.reset();
    if (kind == RequestKind::Result) {
        // A poll that did not arrive cancels neither the engine's ticket nor
        // the wait for it: the next poll asks again.
        edit_notice_ = {InspectorNoticeKind::Error, std::move(message)};
        ++report_revision_;
        return;
    }
    if (kind == RequestKind::Submit) {
        // GL-1d, decision 3: an engine without material_route_default.
        if (retryWithoutRouteDefault(message)) return;
        // A lost submission reply may have committed. Do not offer history
        // until a fresh document is read, or guess which edit it belongs to.
        clearHistory();
        edit_notice_ = {InspectorNoticeKind::Error, std::move(message)};
        ticket_.clear();
        saved_note_.clear();
        // The gesture is gone; the document the canvas shows may not be.
        refresh();
        return;
    }
    failure_ = {InspectorNoticeKind::Error, std::move(message)};
    ++content_revision_;
}

void GraphCanvasModel::receiveRpcResult(std::uint64_t id, std::string_view json) {
    if (scrubbing_ && scrub_render_ == id) {
        // The frame the scrub asked for has been drawn; what it produced is
        // read back the ordinary way, on the read slots.
        scrubbing_ = false;
        scrub_render_ = 0;
        requestObservation();
        ++report_revision_;
        return;
    }
    // EL-5s: get_shader_interface has slots of its own.
    if (shader_interface_reads_.contains(id)) {
        const std::string reply{json};
        takeShaderInterfaceReply(id, &reply, {});
        return;
    }
    // GL-1: a folded view on screen stands aside while the reply is read,
    // and is built again from what the reply left.
    const FoldedSuspend suspend{*this};
    if (const auto slot = takeRead(id)) {
        const auto pending = *slot;
        try {
            const auto response = Json::parse(json);
            if (pending.kind == RequestKind::RerunOnce) {
                receiveRerunOnce(response);
                return;
            }
            // EL-4c: a list the engine failed to build carries its `error`
            // beside status "failed"; that is the list's answer, read below.
            const bool shaders_failed = pending.kind == RequestKind::EngineShaders &&
                                        response.is_object() &&
                                        text(response, "status") == "failed";
            if (response.contains("error") && !shaders_failed) {
                if (pending.kind == RequestKind::Templates ||
                    pending.kind == RequestKind::Authoring ||
                    pending.kind == RequestKind::EngineShaders ||
                    pending.kind == RequestKind::RayLayout)
                    return;
                if (pending.kind == RequestKind::Loops) {
                    // Q-19: an engine with no plan yet is asked again --
                    // GL-1: also when the folded nodes have none to come
                    // from, for as long as an engine takes to build its
                    // first plan.
                    if (plan_passes_.empty() && plan_wait_budget_ > 0) {
                        --plan_wait_budget_;
                        plan_retry_budget_ = std::max(plan_retry_budget_, 1);
                    }
                    plan_retry_wanted_ = plan_retry_budget_ > 0 &&
                                         (planMissesRenderNode() || plan_passes_.empty());
                    return;
                }
                if (pending.kind == RequestKind::LoopPage) {
                    // The loop keeps the materials it has; the next loop
                    // is asked for its own.
                    loop_pages_done_.insert(std::exchange(loop_page_node_, {}));
                    requestNextLoopPage();
                    return;
                }
                if (pending.kind == RequestKind::Catalog) catalog_requested_ = false;
                failure_ = {InspectorNoticeKind::Error, diagnostic(response)};
                ++report_revision_;
                return;
            }
            if (pending.kind == RequestKind::Loops) {
                // Only loops[] is read off the plan; by loop id.
                auto reports = Json::object();
                const auto loops = response.find("loops");
                if (loops != response.end() && loops->is_array())
                    for (const auto &loop : *loops)
                        if (loop.is_object() && !text(loop, "node").empty())
                            reports[text(loop, "node")] = loop;
                loop_reports_ = std::move(reports);
                loop_reports_received_ = true;
                readPlanViews(response);
                readPlanPassInputs(response);
                readPlanPasses(response);
                // GL-1e: and what the rendering section's passes take.
                requestRenderAuthoring();
                plan_retry_wanted_ = plan_retry_budget_ > 0 && planMissesRenderNode();
                // LP-4e: a fresh report starts every loop's pages over.
                loop_pages_done_.clear();
                loop_page_node_.clear();
                requestNextLoopPage();
                ++report_revision_;
                return;
            }
            if (pending.kind == RequestKind::LoopPage) {
                receiveLoopPage(response);
                ++report_revision_;
                return;
            }
            if (pending.kind == RequestKind::EngineShaders) {
                // UX-5d (EL-4c): the engine builds the list behind its frames
                // from start-up and answers at once -- "building" (asked
                // again in half a second, from poll), "failed" with its
                // error, or "ready" with the list. A reply without a status
                // is the list as before.
                const auto status = response.is_object() ? text(response, "status") : std::string{};
                if (status == "building") {
                    engine_shaders_again_at_ =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
                    return;
                }
                if (status == "failed") {
                    takeEngineShaders(Json::array());
                    edit_notice_ = {InspectorNoticeKind::Error,
                                    "The engine could not list its shaders: " +
                                        text(response, "error")};
                    ++report_revision_;
                    return;
                }
                const auto shaders =
                    response.is_array() ? response
                    : response.contains("engine_shaders") ? response.at("engine_shaders")
                                                          : response.value("shaders", Json::array());
                if (shaders.is_array()) takeEngineShaders(shaders);
                ++report_revision_;
                return;
            }
            if (pending.kind == RequestKind::Authoring) {
                auto context = response.is_object() ? response : Json::object();
                auto previous_visuals = render_authoring_;
                auto next_visuals = context;
                // A layout save changes the root digest, not the authoring
                // data drawn by the canvas. Still refresh the editing token
                // and authoring revision, without rebuilding every item.
                previous_visuals.erase("source_digest");
                next_visuals.erase("source_digest");
                const bool changed = previous_visuals != next_visuals;
                takeRenderAuthoring(std::move(context));
                if (changed) ++report_revision_;
                return;
            }
            if (pending.kind == RequestKind::RayLayout) {
                ray_layout_read_ = parseRayLayoutRead(response);
                ++authoring_revision_;
                ++report_revision_;
                return;
            }
            if (pending.kind == RequestKind::Templates) {
                // Only what a feature's fold needs: which feature each
                // template is the decomposed version of, and its document.
                auto templates = Json::object();
                for (const auto &entry : response.value("templates", Json::array())) {
                    if (!entry.is_object()) continue;
                    const auto decomposes = entry.find("decomposes");
                    if (decomposes == entry.end() || !decomposes->is_object()) continue;
                    const auto feature = text(*decomposes, "feature");
                    const auto document = entry.find("document");
                    if (feature.empty() || document == entry.end() || !document->is_object())
                        continue;
                    templates[feature] = *document;
                }
                templates_ = std::move(templates);
                ++report_revision_;
                return;
            }
            if (pending.kind == RequestKind::Catalog) {
                auto operations = Json::object();
                auto types = Json::object();
                for (const auto &module : response.value("modules", Json::array())) {
                    for (const auto &operation : module.value("operations", Json::array()))
                        if (operation.is_object() && operation.contains("op"))
                            operations[text(operation, "op")] = operation;
                    for (const auto &schema : module.value("types", Json::array()))
                        if (schema.is_object() && schema.contains("id"))
                            types[text(schema, "id")] = schema;
                }
                operations_ = std::move(operations);
                types_ = std::move(types);
                // UX-5b (EL-4): the material pass contracts; and the engine's
                // shaders when this engine's catalog still carries them
                // (EL-4b moved them to get_engine_shaders).
                material_pass_contracts_ = response.value("material_pass_contracts", Json::array());
                if (!material_pass_contracts_.is_array()) material_pass_contracts_ = Json::array();
                if (const auto shaders = response.find("engine_shaders");
                    shaders != response.end() && shaders->is_array())
                    takeEngineShaders(*shaders);
                catalog_received_ = true;
                if (engine_shaders_wanted_ && !engine_shaders_known_) askEngineShaders();
            } else if (pending.kind == RequestKind::Status) {
                status_time_ = response.value("time", 0.0);
                status_received_ = true;
                const auto animation = response.find("animation");
                animation_available_ =
                    animation != response.end() && animation->is_object() &&
                    animation->value("available", false);
                animation_sinks_ = animation_available_
                                       ? animation->value("sinks", Json::array())
                                       : Json::array();
            } else if (pending.kind == RequestKind::NodeValues) {
                // The reply names what it answered for. A scope change in
                // flight means the answer in hand is about the other scope,
                // and a value drawn against the wrong node list is worse than
                // no value at all.
                node_values_instance_ = text(response, "instance");
                node_values_ = response.value("nodes", Json::array());
                node_values_published_ = response.value("published", false);
                node_values_received_ = true;
                appendTimelineSample(response.value("frame", std::uint64_t{0}));
            } else if (text(response, "instance") == values_instance_) {
                values_ = response.value("inputs", Json::array());
                values_published_ = response.value("published", false);
                values_received_ = true;
            }
            failure_ = {};
            ++report_revision_;
        } catch (const std::exception &error) {
            if (pending.kind == RequestKind::Loops || pending.kind == RequestKind::Templates ||
                pending.kind == RequestKind::Authoring ||
                pending.kind == RequestKind::EngineShaders)
                return;
            if (pending.kind == RequestKind::LoopPage) {
                loop_pages_done_.insert(std::exchange(loop_page_node_, {}));
                requestNextLoopPage();
                return;
            }
            if (pending.kind == RequestKind::Catalog) catalog_requested_ = false;
            failure_ = {InspectorNoticeKind::Error, error.what()};
            ++report_revision_;
        }
        return;
    }
    if (!pending_ || pending_->id != id) return;
    const auto kind = pending_->kind;
    pending_.reset();
    if (kind == RequestKind::Submit || kind == RequestKind::Result) {
        try {
            const auto response = Json::parse(json);
            if (response.contains("error")) {
                // A CAS mismatch and a named refusal arrive the same way: the
                // gesture is discarded, the reason is shown, and the canvas
                // reads the document again rather than keeping a stale one.
                ticket_.clear();
                saved_note_.clear();
                // GL-1d, decision 3: the same refusal of the route default,
                // should an engine answer it as a rejection.
                if (kind == RequestKind::Submit &&
                    retryWithoutRouteDefault(diagnostic(response)))
                    return;
                // Q-13: a placement refused for a moved document goes again
                // once the document is read (see layout_retry_).
                const auto &error = response.at("error");
                const bool moved =
                    error.is_object() &&
                    error.value("code", std::string{}) == "external_modification";
                if (moved && !std::exchange(layout_retrying_, false) &&
                    pending_history_ &&
                    pending_history_->action == HistoryAction::Edit &&
                    std::ranges::all_of(pending_history_->entry.forward,
                                        [](const Json &op) {
                                            return op.value("op", std::string{}) ==
                                                   "set_layout";
                                        }))
                    layout_retry_ = pending_history_->entry.forward;
                // UX-12a, decision 4: the scope the refused edit was made in.
                std::string edit_scope;
                if (pending_history_)
                    for (const auto &op : pending_history_->entry.forward)
                        if (op.is_object() && op.contains("scope")) {
                            edit_scope = text(op, "scope");
                            if (edit_scope == "root") edit_scope.clear();
                            break;
                        }
                discardPendingHistory();
                edit_notice_ = {InspectorNoticeKind::Error, diagnostic(response)};
                // GL-1b, decision 3: an engine that cannot yet answer a graph
                // edit with restart_required refuses a change to a live
                // material's physical contract with this sentence (and no
                // code). Say what would have applied it. Remove once every
                // engine answers restart_required instead (RS-1).
                if (edit_notice_.message.find(
                        "changes the physical rendering contract of live material") !=
                    std::string::npos)
                    edit_notice_.message += restartNotOfferedHint;
                // LP-4c, decision 9: and where the refusal points.
                markRefusal(error, edit_notice_.message, edit_scope);
                ++content_revision_;
                refresh();
                return;
            }
            if (kind == RequestKind::Submit) {
                if (response.at("status") != "accepted")
                    throw std::invalid_argument("The engine did not accept the graph edit.");
                ticket_ = response.at("ticket").get<std::string>();
                if (ticket_.empty())
                    throw std::invalid_argument("The engine returned no graph edit ticket.");
                poll();
                return;
            }
            const auto status = response.at("status").get<std::string>();
            // Still queued. Nothing on the canvas has changed, so nothing is
            // redrawn: a poll every quarter second must not throw away the
            // drag or the editor the user is in the middle of.
            if (status == "pending") return;
            ticket_.clear();
            if (status != "committed")
                throw std::invalid_argument("The graph edit did not commit: " + status);
            commitHistory(response);
            layout_retrying_ = false;
            refusal_marks_.clear();
            parameter_refusal_.reset();
            scalar_target_fix_.reset();
            // GL-1b, decision 2: written, but the engine can only run it
            // after a restart (RS-1). The owner restarts it; the document and
            // the plan are read from the engine that comes back. RSX: read
            // the way every panel reads a render edit's commit.
            if (auto completion = readRenderEditCompletion(response);
                completion.restart_required) {
                pending_placement_only_ = false;
                edit_notice_ = {InspectorNoticeKind::Information, completion.restart_line};
                engine_restart_ = std::move(completion.restart_line);
                return;
            }
            // PS-1s, decision 2: and what the edit changed besides.
            {
                auto note = std::exchange(saved_note_, {});
                edit_notice_ = {InspectorNoticeKind::Success,
                                note.empty() ? std::string{"Graph change saved."} : std::move(note)};
            }
            // Q-18: the engine published the edit before it said committed,
            // so the plan it holds now is the edit's. The read of the
            // document below brings the plan with it (see receiveRpcResult),
            // and the other panels hear of it through planCommitRevision().
            if (!std::exchange(pending_placement_only_, false)) {
                plan_after_commit_ = true;
                ++plan_commit_revision_;
            }
            refresh();
        } catch (const std::exception &error) {
            ticket_.clear();
            layout_retrying_ = false;
            saved_note_.clear();
            discardPendingHistory();
            edit_notice_ = {InspectorNoticeKind::Error, error.what()};
            refresh();
        }
        return;
    }
    try {
        const auto response = Json::parse(json);
        if (response.contains("error")) {
            failure_ = {InspectorNoticeKind::Error, diagnostic(response)};
            ++content_revision_;
            return;
        }
        auto nodes = response.value("nodes", Json::array());
        if (!nodes.is_array()) throw std::invalid_argument("The graph document has no node array.");
        const auto definitions = response.find("definitions");
        // A reply without definitions is not a broken canvas: the root scope
        // is still the whole document the engine published. Say so and draw it.
        const bool reported = definitions != response.end() && definitions->is_object();
        auto incoming_definitions = reported ? *definitions : Json::object();
        // N-6: the root scope's placement. A reply from an engine that predates
        // it simply says nothing, which reads as "no placement yet".
        auto layout = response.value("layout", Json::object());
        if (!layout.is_object()) layout = Json::object();
        // N-8: an engine that predates this says nothing, which reads as "not a
        // graph root" -- the safe answer, because the placement is then not
        // written rather than written where it cannot be kept.
        auto root_schema = response.value("root_schema", std::string{});
        auto source_reference = response.value("source_reference", std::string{});
        // Q-5: the reply to a `set_layout` -- the canvas's own drag coming
        // back -- changes where things sit and nothing else. Every definition
        // carries its placement inside it, so the comparison takes that member
        // out of both sides before asking whether anything else moved.
        const auto withoutPlacement = [](Json scopes) {
            for (auto &[name, scope] : scopes.items()) {
                (void)name;
                if (scope.is_object()) scope.erase("layout");
            }
            return scopes;
        };
        // GL-5g: the graph itself as the history last read it.
        const bool same_graph = context_received_ && nodes == nodes_ &&
                                withoutPlacement(incoming_definitions) ==
                                    withoutPlacement(definitions_);
        const bool placement_only =
            context_received_ && failure_.kind == InspectorNoticeKind::None &&
            reported == definitions_reported_ && nodes == nodes_ &&
            root_schema == root_schema_ && source_reference == source_reference_ &&
            (layout != layout_ || incoming_definitions != definitions_) &&
            withoutPlacement(incoming_definitions) == withoutPlacement(definitions_);
        definitions_reported_ = reported;
        definitions_ = std::move(incoming_definitions);
        // UX-13b, decision 4 (engine CT-5): who runs each definition.
        definition_instances_ = response.value("definition_instances", Json::object());
        if (!definition_instances_.is_object()) definition_instances_ = Json::object();
        layout_ = std::move(layout);
        nodes_ = std::move(nodes);
        root_schema_ = std::move(root_schema);
        source_reference_ = std::move(source_reference);
        digest_ = response.value("source_digest", Json::object()).value("hex", std::string{});
        if (!history_digest_.empty() && digest_ != history_digest_) {
            const auto restored_digest = std::exchange(render_restore_digest_, std::string{});
            const bool restored = !restored_digest.empty() && digest_ == restored_digest;
            const bool tab_edit = std::exchange(keep_history_through_digest_, false);
            if ((restored || tab_edit) && same_graph)
                history_digest_ = digest_;
            else
                clearHistory();
        }
        context_received_ = true;
        failure_ = {};
        // Q-13: the placement a moved document refused, against this one.
        if (auto retry = std::exchange(layout_retry_, std::nullopt))
            layout_retrying_ = submitOps(std::move(*retry));
        if (placement_only) {
            // The nodes, their ports and the scopes are the ones already
            // drawn, so the values reported against them still stand: asking
            // again would blank every value on the canvas for a round trip.
            ++placement_revision_;
            // Q-18: but a commit of anything else is read into the plan
            // however its document reads back.
            if (std::exchange(plan_after_commit_, false)) {
                plan_retry_budget_ = planRetryLimit;
                plan_wait_budget_ = planWaitLimit;
                requestLoopReports();
            }
            return;
        }
        // The scope trail can name a definition the new document dropped.
        // Q-19: one the edit renamed is followed to its new name.
        const auto renames = std::exchange(committed_renames_, {});
        std::vector<std::string> trail{std::string{}};
        for (std::size_t index = 1; index < breadcrumb_.size(); ++index) {
            auto step = breadcrumb_[index];
            if (!definitions_.contains(step))
                if (const auto renamed = renames.find(step); renamed != renames.end())
                    step = renamed->second;
            if (definitions_.contains(step)) trail.push_back(std::move(step));
        }
        breadcrumb_ = std::move(trail);
        // LP-4c: the loop a body was entered through has to still be there,
        // still running the body on screen.
        if (!entered_loop_.empty()) {
            const auto *loop = findNode({}, entered_loop_);
            const auto bodies = loop != nullptr ? loopBranchBodies(*loop)
                                                : std::vector<std::string>{};
            if (breadcrumb_.size() != 2 ||
                std::ranges::find(bodies, breadcrumb_.back()) == bodies.end())
                entered_loop_.clear();
        }
        ++content_revision_;
        // The document moved under the reported values: ask again rather than
        // showing the previous generation's numbers against new ports.
        values_ = Json::array();
        values_received_ = false;
        values_published_ = false;
        clearObservation();
        ++report_revision_;
        requestValues();
        requestObservation();
        plan_after_commit_ = false;
        plan_retry_budget_ = planRetryLimit;
        plan_wait_budget_ = planWaitLimit;
        requestLoopReports();
    } catch (const std::exception &error) {
        failure_ = {InspectorNoticeKind::Error, error.what()};
        ++content_revision_;
    }
}

std::vector<GraphCanvasScope> GraphCanvasModel::scopes() const {
    std::vector<GraphCanvasScope> result;
    result.push_back(GraphCanvasScope{std::string{}, "root", true});
    for (const auto &[name, definition] : definitions_.items()) {
        (void)definition;
        result.push_back(GraphCanvasScope{name, name, false});
    }
    return result;
}

bool GraphCanvasModel::selectScope(std::string_view scope) {
    // GL-1: the Scope combo names the real document's scopes.
    leaveFolded();
    entered_loop_.clear();
    if (scope.empty()) {
        breadcrumb_ = {std::string{}};
        enteredScope();
        return true;
    }
    if (!definitions_.contains(std::string{scope})) return false;
    breadcrumb_ = {std::string{}, std::string{scope}};
    enteredScope();
    return true;
}

void GraphCanvasModel::enteredScope() {
    // The scope decides which instance the node values answer for, so the one
    // in hand is about the scope that was left.
    clearObservation();
    ++report_revision_;
    requestObservation();
}

std::string GraphCanvasModel::loopEntryBody(const Json &node) const {
    const auto bodies = loopBranchBodies(node);
    const auto chosen = loop_branch_.find(text(node, "id"));
    if (chosen != loop_branch_.end() && chosen->second < bodies.size() &&
        !bodies[chosen->second].empty() && definitions_.contains(bodies[chosen->second]))
        return bodies[chosen->second];
    for (const auto &body : bodies)
        if (!body.empty() && definitions_.contains(body)) return body;
    return {};
}

bool GraphCanvasModel::canEnterInstance(std::string_view node_id) const {
    const auto *node = findNode(currentScope(), node_id);
    if (node == nullptr) return false;
    // LP-4c, decision 5: a loop opens the body its chosen branch runs, the
    // way an instance opens its definition.
    if (currentScope().empty() && isLoop(*node)) return !loopEntryBody(*node).empty();
    if (text(*node, "op") != "graph.instance") return false;
    const auto definition = text(*node, "definition");
    return !definition.empty() && definitions_.contains(definition);
}

bool GraphCanvasModel::enterInstance(std::string_view node_id) {
    if (!canEnterInstance(node_id)) return false;
    // Descending into an instance is also a choice of whose values to watch:
    // the definition body on screen belongs to the instance that was opened.
    // The watch is set before the scope moves and the reads go out after, so
    // one descent is one question rather than the root's and then the body's.
    const auto *node = findNode(currentScope(), node_id);
    if (currentScope().empty() && isLoop(*node)) {
        // A loop owns no scene object: there is no instance to watch, and
        // the body's values are not any one element's.
        const auto body = loopEntryBody(*node);
        const auto bodies = loopBranchBodies(*node);
        loop_branch_[std::string{node_id}] = static_cast<std::size_t>(
            std::distance(bodies.begin(), std::ranges::find(bodies, body)));
        entered_loop_ = std::string{node_id};
        breadcrumb_.push_back(body);
        enteredScope();
        return true;
    }
    const auto definition = text(*node, "definition");
    // GL-1: an instance of a folded view is Studio's drawing, never the
    // engine's: nothing answers for its values.
    const bool watch = !folded_stash_ && currentScope().empty() &&
                       node->find("owner") != node->end() &&
                       values_instance_ != node_id;
    if (watch) setWatchedInstance(std::string{node_id});
    breadcrumb_.push_back(definition);
    enteredScope();
    if (watch) requestValues();
    return true;
}

bool GraphCanvasModel::leaveScope() {
    // GL-1: out of a folded view one step at a time -- the scope entered
    // inside it first, then the folded steps, then back to the real root.
    if (breadcrumb_.size() <= 1 && folded_stash_) {
        folded_trail_.pop_back();
        if (folded_trail_.empty() || !showFoldedTop()) leaveFolded();
        enteredScope();
        return true;
    }
    if (breadcrumb_.size() <= 1) return false;
    breadcrumb_.pop_back();
    entered_loop_.clear();
    enteredScope();
    return true;
}

std::size_t GraphCanvasModel::loopBranch(std::string_view id) const {
    const auto *node = findNode({}, id);
    if (node == nullptr || !isLoop(*node)) return 0;
    const auto bodies = loopBranchBodies(*node);
    // UX-1, decision 4: the branch the tab chose, even when an earlier
    // branch runs the same body -- found by its body, the tab of the else
    // could never be the one shown as chosen.
    if (const auto chosen = loop_branch_.find(std::string{id});
        chosen != loop_branch_.end() && chosen->second < bodies.size() &&
        !bodies[chosen->second].empty() && definitions_.contains(bodies[chosen->second]))
        return chosen->second;
    const auto body = loopEntryBody(*node);
    const auto found = std::ranges::find(bodies, body);
    return found == bodies.end() ? 0 : static_cast<std::size_t>(found - bodies.begin());
}

bool GraphCanvasModel::selectLoopBranch(std::size_t branch) {
    if (entered_loop_.empty() || breadcrumb_.size() != 2) return false;
    const auto *node = findNode({}, entered_loop_);
    if (node == nullptr) return false;
    const auto bodies = loopBranchBodies(*node);
    if (branch >= bodies.size() || bodies[branch].empty() ||
        !definitions_.contains(bodies[branch]))
        return false;
    loop_branch_[entered_loop_] = branch;
    if (breadcrumb_.back() == bodies[branch]) return true;
    breadcrumb_.back() = bodies[branch];
    enteredScope();
    return true;
}

std::string GraphCanvasModel::loopZoneBody(std::string_view loop) const {
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return {};
    return loopEntryBody(*node);
}

bool GraphCanvasModel::showLoopBranch(std::string_view loop, std::size_t branch) {
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return false;
    const auto bodies = loopBranchBodies(*node);
    if (branch >= bodies.size() || bodies[branch].empty() ||
        !definitions_.contains(bodies[branch]))
        return false;
    loop_branch_[std::string{loop}] = branch;
    return true;
}

std::vector<GraphCanvasBodyOutput> GraphCanvasModel::bodyOutputs(std::string_view definition) const {
    std::vector<GraphCanvasBodyOutput> result;
    const auto found = definitions_.find(std::string{definition});
    if (found == definitions_.end() || !found->is_object()) return result;
    const auto outputs = found->find("outputs");
    if (outputs == found->end() || !outputs->is_object()) return result;
    for (const auto &[name, endpoint] : outputs->items())
        result.push_back(GraphCanvasBodyOutput{
            name, endpoint.is_object() ? text(endpoint, "node") : std::string{},
            endpoint.is_object() ? text(endpoint, "port") : std::string{}});
    return result;
}

std::size_t GraphCanvasModel::definitionUseCount(std::string_view definition) const {
    std::size_t uses = 0;
    for (const auto &node : nodes_) {
        if (!node.is_object()) continue;
        if (text(node, "op") == "graph.instance" && text(node, "definition") == definition)
            ++uses;
        // Q-17: a loop is one user however many of its branches run the
        // definition. Two branches of one loop are one place to edit it from;
        // "shared" warns of an edit reaching another loop or instance.
        if (isLoop(node) && std::ranges::any_of(loopBranchBodies(node), [&](const auto &body) {
                return body == definition;
            }))
            ++uses;
    }
    return uses;
}

bool GraphCanvasModel::enterLoopOrigin(std::string_view loop, std::int64_t element,
                                       std::string_view node) {
    // GL-1: a copy of the real document's loop, never a folded view's.
    leaveFolded();
    const auto *found = findNode({}, loop);
    if (found == nullptr || !isLoop(*found)) return false;
    const auto bodies = loopBranchBodies(*found);
    const auto branch = branchForElement(loop, element);
    if (!branch || *branch >= bodies.size() || bodies[*branch].empty()) return false;
    if (findNode(bodies[*branch], node) == nullptr) return false;
    breadcrumb_ = {std::string{}};
    loop_branch_[std::string{loop}] = *branch;
    entered_loop_ = std::string{loop};
    breadcrumb_.push_back(bodies[*branch]);
    enteredScope();
    return true;
}

std::optional<std::size_t> GraphCanvasModel::branchForElement(std::string_view loop,
                                                              std::int64_t element) const {
    if (const auto *report = loopReport(loop)) {
        const auto elements = report->find("elements");
        if (elements != report->end() && elements->is_array())
            for (const auto &entry : *elements)
                // LP-4e: a draw_loop's element is a material's key, never
                // an integer; value() would throw on it.
                if (entry.is_object() && entry.contains("element") &&
                    entry.at("element").is_number_integer() &&
                    entry.at("element").get<std::int64_t>() == element &&
                    entry.contains("branch") && entry.at("branch").is_number_integer() &&
                    entry.at("branch").get<std::int64_t>() >= 0)
                    return static_cast<std::size_t>(entry.at("branch").get<std::int64_t>());
    }
    // No report: the conditions the table can read are integers, so they
    // can be asked here. An expression stops the walk -- guessing past it
    // would be naming a branch the engine may not run.
    const auto *node = findNode({}, loop);
    if (node == nullptr) return std::nullopt;
    const auto parameters = node->value("parameters", Json::object());
    const auto branches = parameters.value("branches", Json::array());
    for (std::size_t index = 0; index < branches.size(); ++index) {
        const auto read = readLoopBranch(branches[index]);
        if (read.when == "else") return index;
        if (read.when == "other" || read.when == "draw") return std::nullopt;
        const bool listed = std::ranges::find(read.values, element) != read.values.end();
        if ((read.when == "element" || read.when == "element_in") == listed) return index;
    }
    return std::nullopt;
}

const Json *GraphCanvasModel::scopeNodes(std::string_view scope) const {
    if (scope.empty()) return &nodes_;
    const auto found = definitions_.find(std::string{scope});
    if (found == definitions_.end()) return nullptr;
    const auto nodes = found->find("nodes");
    if (nodes == found->end() || !nodes->is_array()) return nullptr;
    return &*nodes;
}

const Json *GraphCanvasModel::findNode(std::string_view scope, std::string_view id) const {
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return nullptr;
    for (const auto &node : *nodes)
        if (node.is_object() && std::string_view{text(node, "id")} == id) return &node;
    return nullptr;
}

const Json *GraphCanvasModel::nodeOutputs(std::string_view scope,
                                          std::string_view id) const {
    if (!node_values_received_ || !node_values_published_) return nullptr;
    // LP-4c: a loop's body is no instance's; the watched instance's values
    // are about another body, whatever node ids the two share.
    if (!scope.empty() && !entered_loop_.empty()) return nullptr;
    // LP-4d1: nor is a body drawn in a zone of the root: the reply in hand is
    // about the scope on screen, whatever node ids the two share.
    if (scope != currentScope()) return nullptr;
    // The reply answers one question. If the scope on screen would ask a
    // different one, the answer in hand is not about these nodes.
    const auto expected = scope.empty() ? std::string{} : values_instance_;
    if (node_values_instance_ != expected) return nullptr;
    for (const auto &reported : node_values_) {
        if (!reported.is_object()) continue;
        if (std::string_view{text(reported, "node")} != id) continue;
        const auto outputs = reported.find("outputs");
        if (outputs == reported.end() || !outputs->is_object()) return nullptr;
        return &*outputs;
    }
    return nullptr;
}

void GraphCanvasModel::appendTimelineSample(std::uint64_t frame) {
    if (!live_ || timeline_node_.empty()) return;
    const auto *outputs = nodeOutputs(currentScope(), timeline_node_);
    if (outputs == nullptr) return;
    // The port list is fixed by the first sample of a run: a line that changed
    // which port it meant halfway along would be two lines drawn as one.
    if (timeline_ports_.empty())
        for (const auto &[port, value] : outputs->items()) {
            if (!value.is_number()) continue;
            if (timeline_ports_.size() >= timelineTraceLimit) break;
            timeline_ports_.push_back(port);
        }
    if (timeline_ports_.empty()) return;

    GraphCanvasTimelineSample sample;
    sample.time = status_time_;
    sample.frame = frame;
    sample.values.reserve(timeline_ports_.size());
    for (const auto &port : timeline_ports_) {
        const auto found = outputs->find(port);
        sample.values.push_back(found != outputs->end() && found->is_number()
                                    ? found->get<double>()
                                    : 0.0);
    }
    // The animation band, when the watched instance owns a sink. The phase and
    // the transition are the two readings that mean anything on a time axis.
    for (const auto &sink : animationSinks()) {
        if (!sink.cursors.empty()) {
            sample.has_phase = true;
            sample.phase = sink.cursors.front().normalized_phase;
        }
        if (sink.transition_active) {
            sample.has_progress = true;
            sample.progress = sink.transition_progress;
        }
        break;
    }
    timeline_.push_back(std::move(sample));
    if (timeline_.size() > timelineCapacity)
        timeline_.erase(timeline_.begin(),
                        timeline_.begin() +
                            static_cast<std::ptrdiff_t>(timeline_.size() -
                                                        timelineCapacity));
}

std::vector<GraphCanvasAnimationSink> GraphCanvasModel::animationSinks() const {
    std::vector<GraphCanvasAnimationSink> found;
    if (!animation_available_ || values_instance_.empty()) return found;
    // The owner is the instance's, read off the document. An instance with no
    // owner owns no sink, so there is nothing to match against and nothing to
    // draw -- which is not the same as "the engine reported none".
    const auto *instance = findNode({}, values_instance_);
    if (instance == nullptr) return found;
    const auto owner = instance->find("owner");
    if (owner == instance->end() || !owner->is_object()) return found;
    const auto scene = text(*owner, "scene");
    const auto object = owner->value("object", std::uint64_t{0});
    for (const auto &reported : animation_sinks_) {
        if (!reported.is_object()) continue;
        const auto sink_owner = reported.find("owner");
        if (sink_owner == reported.end() || !sink_owner->is_object()) continue;
        if (text(*sink_owner, "scene") != scene) continue;
        if (sink_owner->value("object", std::uint64_t{0}) != object) continue;
        GraphCanvasAnimationSink sink;
        sink.node = text(reported, "node");
        sink.source = text(reported, "source");
        sink.state = text(reported, "state");
        const auto transition = reported.find("transition");
        if (transition != reported.end() && transition->is_object()) {
            sink.transition_active = transition->value("active", false);
            sink.transition_target = text(*transition, "target");
            sink.transition_progress = transition->value("progress", 0.0);
        }
        for (const auto &cursor : reported.value("cursors", Json::array())) {
            if (!cursor.is_object()) continue;
            sink.cursors.push_back(GraphCanvasAnimationSink::Cursor{
                text(cursor, "clip"), cursor.value("time_seconds", 0.0),
                cursor.value("normalized_phase", 0.0)});
        }
        found.push_back(std::move(sink));
    }
    return found;
}

const Json *GraphCanvasModel::operation(std::string_view op) const {
    const auto found = operations_.find(std::string{op});
    return found == operations_.end() ? nullptr : &*found;
}

const Json *GraphCanvasModel::typeSchema(std::string_view id) const {
    const auto found = types_.find(std::string{id});
    return found == types_.end() ? nullptr : &*found;
}

std::string GraphCanvasModel::definitionOutputType(std::string_view definition,
                                                   std::string_view port,
                                                   int depth) const {
    if (depth <= 0) return {};
    const auto body = definitions_.find(std::string{definition});
    if (body == definitions_.end()) return {};
    const auto outputs = body->find("outputs");
    if (outputs == body->end() || !outputs->is_object()) return {};
    const auto endpoint = outputs->find(std::string{port});
    if (endpoint == outputs->end() || !endpoint->is_object()) return {};
    const auto producer = text(*endpoint, "node");
    const auto producer_port = text(*endpoint, "port");
    const auto *node = findNode(definition, producer);
    if (node == nullptr) return {};
    const auto op = text(*node, "op");
    if (op == "graph.input") return kGraphInputType;
    if (op == "graph.instance")
        return definitionOutputType(text(*node, "definition"), producer_port,
                                    depth - 1);
    // LP-4e: parameter_dependent, but always the same one output.
    if (isDrawStep(op)) return std::string{kDrawStreamType};
    const auto *contract = operation(op);
    if (contract == nullptr) return {};
    for (const auto &output : contract->value("outputs", Json::array()))
        if (text(output, "port") == producer_port)
            return semanticTypeName(output, "type");
    return {};
}

std::vector<GraphCanvasNode> GraphCanvasModel::nodes(std::string_view scope) const {
    std::vector<GraphCanvasNode> result;
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return result;
    const bool root = scope.empty();
    for (const auto &node : *nodes) {
        if (!node.is_object()) continue;
        GraphCanvasNode drawn;
        drawn.id = text(node, "id");
        drawn.op = text(node, "op");
        drawn.label = text(node, "label");
        drawn.definition = text(node, "definition");
        drawn.instance = drawn.op == "graph.instance";
        // LP-4c: a loop stands only in the root document, where its ports
        // are its body's boundary.
        drawn.loop = root && drawn.op == "graph.loop";
        // The body the ports are read from: the first branch that has one,
        // which is the boundary every branch has to share.
        std::string port_body;
        if (drawn.loop) {
            for (const auto &body : loopBranchBodies(node))
                if (!body.empty() && definitions_.contains(body)) {
                    port_body = body;
                    break;
                }
            drawn.definition = loopEntryBody(node);
            // No catalog entry names it (design §8): it is structure, as
            // the palette files it, not an operation the catalog missed.
            drawn.contract_known = true;
            drawn.domain = "structure";
            drawn.evaluation = "direct";
            drawn.badge = "↻ loop";
            if (const auto *report = loopReport(drawn.id)) {
                const auto tier = text(*report, "tier");
                const auto count = report->value("count", std::int64_t{0});
                if (!tier.empty())
                    drawn.badge = "↻ " + tier + " ×" + std::to_string(count);
            }
        }
        for (const auto &mark : refusal_marks_)
            if (mark.scope == scope && mark.node == drawn.id) {
                drawn.refused = true;
                drawn.refusal = mark.reason;
            }
        const auto parameters = node.find("parameters");
        if (parameters != node.end() && parameters->is_object()) drawn.parameters = *parameters;
        const auto owner = node.find("owner");
        if (owner != node.end() && owner->is_object()) {
            drawn.has_owner = true;
            drawn.owner_scene = text(*owner, "scene");
            drawn.owner_object = owner->value("object", std::uint64_t{0});
        }

        const auto *contract = operation(drawn.op);
        const bool declared = contract != nullptr && text(*contract, "ports") == "declared";
        if (contract != nullptr) {
            drawn.contract_known = true;
            drawn.domain = text(*contract, "domain");
            drawn.evaluation = text(*contract, "evaluation");
            drawn.sink = contract->value("sink", false);
        }

        const auto authored_inputs = node.find("inputs");
        const bool has_authored_inputs =
            authored_inputs != node.end() && authored_inputs->is_object();

        std::vector<GraphCanvasPort> ports;
        const auto add_port = [&](std::string name, bool from_contract,
                                  std::string type = {}) {
            if (name.empty()) return;
            if (std::ranges::any_of(ports, [&](const auto &existing) {
                    return existing.port == name;
                }))
                return;
            GraphCanvasPort port;
            port.port = std::move(name);
            port.declared = from_contract;
            port.type = std::move(type);
            ports.push_back(std::move(port));
        };

        if (drawn.instance && definitions_.contains(drawn.definition)) {
            // graph.instance is parameter_dependent: the catalog declares no
            // ports for it, so the definition's own graph.input nodes are the
            // port list, in the order the definition declares them.
            for (auto &port : definitionInputs(definitions_.at(drawn.definition)))
                add_port(std::move(port), false, kGraphInputType);
        } else if (drawn.loop) {
            // LP-4c: the body's graph.input nodes, like an instance's. What
            // flows in is whatever the body reads it as -- a render resource
            // as often as a scalar -- so no type is claimed for it. The
            // parameters (over, branches, ...) are the Loop fields, not ports.
            if (!port_body.empty())
                for (auto &port : definitionInputs(definitions_.at(port_body)))
                    add_port(std::move(port), false);
        } else if (drawn.op == kMaterialPass || isDrawStep(drawn.op)) {
            // LP-4e: parameter_dependent, but the engine's contract is short
            // enough to restate: a material pass's sockets follow its
            // contract (its attachments, then depth, then the draws a
            // draw_loop hands it); a draw step follows the one on `after`.
            // The parameters are fields, never ports.
            const auto socket = [&](std::string name, bool required, std::string type) {
                GraphCanvasPort port;
                port.port = std::move(name);
                port.declared = true;
                port.required = required;
                port.type = std::move(type);
                ports.push_back(std::move(port));
            };
            if (drawn.op == kMaterialPass) {
                for (auto &name : materialPassColorSockets(drawn.parameters))
                    socket(std::move(name), true, {});
                socket("depth", true, {});
                socket("draws", false, std::string{kDrawStreamType});
            } else {
                socket("after", false, std::string{kDrawStreamType});
            }
        } else if (declared) {
            for (const auto &input : contract->value("inputs", Json::array())) {
                GraphCanvasPort port;
                port.port = text(input, "port");
                if (port.port.empty()) continue;
                port.declared = true;
                port.required = input.value("required", false);
                if (input.contains("accepted_type")) port.accepted_type = input.at("accepted_type").dump();
                port.type = semanticTypeName(input, "accepted_type");
                ports.push_back(std::move(port));
            }
        } else if (drawn.parameters.is_object()) {
            // Neither a contract nor a definition to read: the node's own keys
            // are all there is, and they are labelled as exactly that.
            for (const auto &[name, value] : drawn.parameters.items()) {
                (void)value;
                add_port(name, false);
            }
        }
        // UX-8, decision 1: a buffer takes its value on an edge (graphmodule's
        // renderPorts gives render.buffer and render.attribute an optional
        // scalar `value`), which the catalog cannot list for an op whose
        // ports follow its parameters. It is the first row, typed, so a
        // curve's line lands there and nothing else's does.
        if (!declared && (drawn.op == "render.buffer" || drawn.op == "render.attribute") &&
            std::ranges::none_of(ports, [](const auto &port) { return port.port == "value"; })) {
            GraphCanvasPort value;
            value.port = "value";
            value.declared = true;
            value.type = kGraphInputType;
            ports.insert(ports.begin(), std::move(value));
        }
        if (has_authored_inputs)
            for (const auto &[name, value] : authored_inputs->items()) {
                (void)value;
                add_port(name, false);
            }

        for (auto &port : ports) {
            if (has_authored_inputs) {
                const auto edge = authored_inputs->find(port.port);
                if (edge != authored_inputs->end() && edge->is_object()) {
                    port.connected = true;
                    port.source_node = text(*edge, "node");
                    port.source_port = text(*edge, "port");
                    if (const auto element = edge->find("element"); element != edge->end())
                        port.source_element = element->dump();
                }
            }
            const auto stored = drawn.loop ? drawn.parameters.end()
                                           : drawn.parameters.find(port.port);
            if (stored != drawn.parameters.end()) {
                port.has_parameter = true;
                port.parameter = *stored;
            }
            // Only a root instance can carry published values; a node inside a
            // definition body has no RPC that answers for it (AG-3).
            if (!root || !drawn.instance || drawn.id != values_instance_ || !values_received_)
                continue;
            for (const auto &reported : values_) {
                if (text(reported, "port") != port.port) continue;
                const auto value = reported.find("value");
                if (value != reported.end() && value->is_number()) {
                    port.has_value = true;
                    port.value = value->get<double>();
                }
                port.value_source = text(reported, "source");
                const auto connected = reported.find("connected");
                if (connected != reported.end() && connected->is_object()) {
                    port.value_connected_node = text(*connected, "node");
                    port.value_connected_port = text(*connected, "port");
                }
                break;
            }
        }
        drawn.inputs = std::move(ports);
        // EL-5s, decision 1: a compute node's sampled images its shader
        // declares and no line feeds yet, as empty sockets to drop one on.
        if (drawn.op == "render.compute")
            for (auto &open : computeOpenSockets(scope, drawn.id)) {
                GraphCanvasPort socket;
                socket.port = std::move(open);
                socket.declared = true;
                socket.required = true;
                drawn.inputs.push_back(std::move(socket));
            }
        // Q-19, decision 1: a render node whose pass published its shader's
        // declarations draws them as its input sockets, and its numbered
        // ports are theirs rather than rows of their own. Without them (no
        // pipeline yet, a shader that declares nothing) the rows stay.
        if (isGraphicsPass(drawn.op))
            if (const auto *declared = passInputs(scope, drawn.id);
                declared != nullptr && declaresPassInputs(*declared)) {
                drawn.declared_sockets = true;
                drawn.input_sockets = graphInputSockets(*declared, node, bufferPortReads(scope, drawn.id));
                const auto numbered = numberedPorts(drawn.parameters);
                std::erase_if(drawn.inputs, [&](const GraphCanvasPort &port) {
                    return std::ranges::any_of(numbered, [&](const auto &entry) {
                               return entry.second == port.port;
                           }) ||
                           // UX-8: a buffer port a declaration's socket shows.
                           std::ranges::any_of(drawn.input_sockets, [&](const auto &socket) {
                               return socket.buffer && socket.port == port.port;
                           });
                });
            }

        if (drawn.instance && definitions_.contains(drawn.definition)) {
            const auto &definition = definitions_.at(drawn.definition);
            const auto outputs = definition.find("outputs");
            if (outputs != definition.end() && outputs->is_object())
                for (const auto &[name, target] : outputs->items()) {
                    (void)target;
                    drawn.outputs.push_back(GraphCanvasOutput{
                        name, definitionOutputType(drawn.definition, name, 8)});
                }
        } else if (drawn.loop) {
            // LP-4c: what the loop declares, `p` (the last element's) and
            // `p[]` (one per element), each typed by the body's output `p`.
            const auto authored = node.find("outputs");
            if (authored != node.end() && authored->is_array())
                for (const auto &output : *authored) {
                    if (!output.is_string()) continue;
                    GraphCanvasOutput drawn_output;
                    drawn_output.port = output.get<std::string>();
                    drawn_output.bundle = drawn_output.port.ends_with("[]");
                    const auto base = drawn_output.bundle
                                          ? drawn_output.port.substr(0, drawn_output.port.size() - 2)
                                          : drawn_output.port;
                    if (!port_body.empty())
                        drawn_output.type = definitionOutputType(port_body, base, 8);
                    drawn.outputs.push_back(std::move(drawn_output));
                }
        } else if (isDrawStep(drawn.op)) {
            // LP-4e: a draw step hands the list on.
            drawn.outputs.push_back(GraphCanvasOutput{"draws", std::string{kDrawStreamType}});
        } else if (drawn.op == kMaterialPass) {
            // GL-1f (RS-4): what it wrote, handed on as an output of each
            // attachment socket's name, typed as the image on that socket.
            // A reader adds the port to the node's `outputs` (withWriterOutputs).
            auto names = materialPassColorSockets(drawn.parameters);
            names.push_back("depth");
            for (auto &name : names) {
                const auto type = forwardedImageType(scope, drawn.id, name, 8);
                drawn.outputs.push_back(GraphCanvasOutput{
                    std::move(name), type.is_object() ? text(type, "semantic") : std::string{}});
            }
        } else if (drawn.op == "graph.input") {
            // The structural boundary port. Its single output is the scalar
            // the expansion produces, which no catalog entry can say.
            drawn.outputs.push_back(GraphCanvasOutput{"value", kGraphInputType});
        } else if (declared) {
            for (const auto &output : contract->value("outputs", Json::array())) {
                const auto name = text(output, "port");
                if (name.empty()) continue;
                // GL-1f (RS-4): `type_from_input` -- no type of its own; the
                // type of the edge into that input (shadow_depth's depth,
                // snapshot_copy's destination).
                if (const auto from = text(output, "type_from_input"); !from.empty()) {
                    const auto type = forwardedImageType(scope, drawn.id, from, 8);
                    drawn.outputs.push_back(GraphCanvasOutput{
                        name, type.is_object() ? text(type, "semantic") : std::string{}});
                    continue;
                }
                drawn.outputs.push_back(GraphCanvasOutput{name, semanticTypeName(output, "type")});
            }
        } else {
            const auto authored = node.find("outputs");
            if (authored != node.end() && authored->is_array())
                for (const auto &output : *authored)
                    if (output.is_string())
                        drawn.outputs.push_back(
                            GraphCanvasOutput{output.get<std::string>(), {}});
        }

        // AG-3b: what the published generation left on the outputs. This is
        // the one report that reaches a definition body, so it is applied in
        // every scope rather than only to a root instance.
        if (const auto *outputs = nodeOutputs(scope, drawn.id))
            for (auto &output : drawn.outputs) {
                const auto found = outputs->find(output.port);
                if (found == outputs->end() || !found->is_number()) continue;
                output.has_value = true;
                output.value = found->get<double>();
            }
        result.push_back(std::move(drawn));
    }
    return result;
}

std::vector<GraphCanvasEdge> GraphCanvasModel::edges(std::string_view scope) const {
    std::vector<GraphCanvasEdge> result;
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return result;
    std::set<std::string, std::less<>> present;
    for (const auto &node : *nodes)
        if (node.is_object()) present.insert(text(node, "id"));
    for (const auto &node : *nodes) {
        if (!node.is_object()) continue;
        const auto inputs = node.find("inputs");
        if (inputs == node.end() || !inputs->is_object()) continue;
        for (const auto &[port, edge] : inputs->items()) {
            if (!edge.is_object()) continue;
            const auto producer = text(edge, "node");
            // An edge to a node this scope does not hold would draw a line to
            // nowhere; the document says where it goes, not this canvas.
            if (!present.contains(producer)) continue;
            GraphCanvasEdge drawn{producer, text(edge, "port"), text(node, "id"), port};
            // LP-4d2, decision 4: which element of a `p[]` the edge reads, as
            // its label shows it -- an expression by its text, a literal as
            // written.
            if (const auto element = edge.find("element"); element != edge.end()) {
                if (element->is_object() && element->contains("$expr") &&
                    element->at("$expr").is_string())
                    drawn.element = element->at("$expr").get<std::string>();
                else if (element->is_string())
                    drawn.element = element->get<std::string>();
                else
                    drawn.element = element->dump();
            }
            result.push_back(std::move(drawn));
        }
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> GraphCanvasModel::allNodeKeys() const {
    std::vector<std::pair<std::string, std::string>> keys;
    for (const auto &scope : scopes()) {
        const auto *nodes = scopeNodes(scope.id);
        if (nodes == nullptr) continue;
        for (const auto &node : *nodes)
            if (node.is_object()) keys.emplace_back(scope.id, text(node, "id"));
    }
    return keys;
}

std::vector<std::string> GraphCanvasModel::valueInstances() const {
    std::vector<std::string> instances;
    for (const auto &node : nodes_) {
        if (!node.is_object() || text(node, "op") != "graph.instance") continue;
        const auto owner = node.find("owner");
        if (owner == node.end() || !owner->is_object()) continue;
        instances.push_back(text(node, "id"));
    }
    return instances;
}

std::string GraphCanvasModel::sceneTimeNode(std::string_view scope) const {
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return {};
    for (const auto &node : *nodes) {
        if (!node.is_object() || text(node, "op") != "value.input") continue;
        const auto parameters = node.find("parameters");
        if (parameters == node.end() || !parameters->is_object()) continue;
        if (text(*parameters, "name") == "scene_time") return text(node, "id");
    }
    return {};
}

std::string GraphCanvasModel::scopeToken() const {
    return scopeToken(currentScope());
}

std::string GraphCanvasModel::scopeToken(std::string_view scope) {
    // The engine spells the root document "root"; the canvas spells it with the
    // empty id so a definition named "root" is still a scope of its own.
    return scope.empty() ? std::string{"root"} : std::string{scope};
}

bool GraphCanvasModel::submitOps(Json ops) {
    if (!canEdit()) return false;
    if (!ops.is_array() || ops.empty()) return false;
    if (folded_stash_) return refuseFoldedEdit(ops);
    // GL-1f: a pass's forwarded output is listed in its `outputs` while
    // something reads it, in the same list (one Undo).
    ops = withWriterOutputs(std::move(ops));
    return submitHistory(HistoryAction::Edit, {ops, inverseOps(ops)});
}

Json GraphCanvasModel::withWriterOutputs(Json ops) const {
    // The document as the ops leave it, walked op by op: enough of it to know
    // who reads what (nodes and their inputs, definitions' outputs).
    Json nodes = nodes_;
    Json definitions = definitions_.is_object() ? definitions_ : Json::object();
    const auto scopeNodes = [&](const std::string &scope) -> Json * {
        if (scope.empty()) return &nodes;
        const auto found = definitions.find(scope);
        if (found == definitions.end() || !found->is_object()) return nullptr;
        if (!found->contains("nodes") || !found->at("nodes").is_array())
            (*found)["nodes"] = Json::array();
        return &found->at("nodes");
    };
    const auto locate = [&](const std::string &scope, const std::string &id) -> Json * {
        auto *list = scopeNodes(scope);
        if (list == nullptr) return nullptr;
        for (auto &node : *list)
            if (node.is_object() && text(node, "id") == id) return &node;
        return nullptr;
    };
    const auto scopeOf = [](const Json &op) {
        const auto scope = op.contains("scope") && op.at("scope").is_string()
                               ? op.at("scope").get<std::string>()
                               : std::string{"root"};
        return scope == "root" ? std::string{} : scope;
    };
    const auto tokenOf = [](const std::string &scope) {
        return scope.empty() ? std::string{"root"} : scope;
    };
    // The forwarded outputs an edit stopped reading (scope, node, port).
    std::set<std::tuple<std::string, std::string, std::string>> released;
    Json result = Json::array();
    // Before an op that reads `source` in `scope`: the writer lists the port.
    const auto list = [&](const std::string &scope, const Json &source) {
        if (!source.is_object()) return;
        const auto writer = text(source, "node");
        const auto port = text(source, "port");
        auto *node = locate(scope, writer);
        if (node == nullptr || forwardedInputOf(*node, port).empty() || listsOutput(*node, port))
            return;
        if (!node->contains("outputs") || !node->at("outputs").is_array())
            (*node)["outputs"] = Json::array();
        (*node)["outputs"].push_back(port);
        result.push_back(Json{{"op", "replace_node"}, {"scope", tokenOf(scope)}, {"node", *node}});
    };
    const auto release = [&](const std::string &scope, const Json &source) {
        if (source.is_object())
            released.emplace(scope, text(source, "node"), text(source, "port"));
    };
    for (auto &op : ops) {
        if (!op.is_object()) {
            result.push_back(op);
            continue;
        }
        const auto kind = text(op, "op");
        const auto scope = scopeOf(op);
        if (kind == "set_input") {
            auto *node = locate(scope, text(op, "node"));
            const auto port = text(op, "port");
            if (node != nullptr)
                if (const auto inputs = node->find("inputs");
                    inputs != node->end() && inputs->is_object())
                    if (const auto was = inputs->find(port); was != inputs->end())
                        release(scope, *was);
            list(scope, op.value("source", Json{}));
            result.push_back(op);
            if (node == nullptr) node = locate(scope, text(op, "node"));
            if (node != nullptr) {
                const auto &source = op.value("source", Json{});
                if (source.is_null()) {
                    if (node->contains("inputs") && node->at("inputs").is_object())
                        (*node)["inputs"].erase(port);
                } else {
                    (*node)["inputs"][port] = source;
                }
            }
            continue;
        }
        if (kind == "add_node" && op.contains("node") && op.at("node").is_object()) {
            const auto inputs = op.at("node").value("inputs", Json::object());
            if (inputs.is_object())
                for (const auto &[name, source] : inputs.items()) {
                    (void)name;
                    list(scope, source);
                }
            result.push_back(op);
            if (auto *list_nodes = scopeNodes(scope)) list_nodes->push_back(op.at("node"));
            continue;
        }
        if (kind == "remove_node") {
            auto *list_nodes = scopeNodes(scope);
            const auto id = text(op, "node");
            if (list_nodes != nullptr) {
                for (const auto &node : *list_nodes) {
                    if (!node.is_object() || text(node, "id") != id) continue;
                    const auto inputs = node.value("inputs", Json::object());
                    for (const auto &[name, source] : inputs.items()) {
                        (void)name;
                        release(scope, source);
                    }
                }
                Json kept = Json::array();
                for (auto &node : *list_nodes)
                    if (!(node.is_object() && text(node, "id") == id)) kept.push_back(node);
                *list_nodes = std::move(kept);
            }
            result.push_back(op);
            continue;
        }
        if (kind == "replace_node" && op.contains("node") && op.at("node").is_object()) {
            if (auto *node = locate(scope, text(op.at("node"), "id"))) *node = op.at("node");
            result.push_back(op);
            continue;
        }
        if (kind == "set_outputs" && !scope.empty() && op.contains("outputs") &&
            op.at("outputs").is_object()) {
            for (const auto &[name, source] : op.at("outputs").items()) {
                (void)name;
                list(scope, source);
            }
            if (const auto found = definitions.find(scope);
                found != definitions.end() && found->is_object()) {
                const auto previous = found->value("outputs", Json::object());
                for (const auto &[name, source] : previous.items()) {
                    (void)name;
                    release(scope, source);
                }
                (*found)["outputs"] = op.at("outputs");
            }
            result.push_back(op);
            continue;
        }
        if (kind == "add_definition") {
            definitions[text(op, "name")] = op.value("definition", Json::object());
        } else if (kind == "remove_definition") {
            definitions.erase(text(op, "name"));
        } else if (kind == "rename_definition") {
            const auto from = text(op, "from");
            if (const auto found = definitions.find(from); found != definitions.end()) {
                auto body = *found;
                definitions.erase(from);
                definitions[text(op, "to")] = std::move(body);
            }
        }
        result.push_back(op);
    }
    // After the list: a released port nothing in its scope reads any more
    // leaves the writer's `outputs` (the engine would take it either way).
    for (const auto &[scope, writer, port] : released) {
        auto *node = locate(scope, writer);
        if (node == nullptr || forwardedInputOf(*node, port).empty() || !listsOutput(*node, port))
            continue;
        const auto reads = [&](const Json &source) {
            return source.is_object() && text(source, "node") == writer &&
                   text(source, "port") == port;
        };
        bool read = false;
        if (const auto *list_nodes = scopeNodes(scope))
            for (const auto &other : *list_nodes) {
                if (!other.is_object()) continue;
                const auto inputs = other.value("inputs", Json::object());
                for (const auto &[name, source] : inputs.items()) {
                    (void)name;
                    read = read || reads(source);
                }
            }
        if (!scope.empty())
            if (const auto found = definitions.find(scope);
                found != definitions.end() && found->is_object()) {
                const auto outputs = found->value("outputs", Json::object());
                for (const auto &[name, source] : outputs.items()) {
                    (void)name;
                    read = read || reads(source);
                }
            }
        if (read) continue;
        Json kept = Json::array();
        for (const auto &output : (*node)["outputs"])
            if (!(output.is_string() && output.get<std::string>() == port)) kept.push_back(output);
        (*node)["outputs"] = std::move(kept);
        result.push_back(Json{{"op", "replace_node"}, {"scope", tokenOf(scope)}, {"node", *node}});
    }
    return result;
}

Json GraphCanvasModel::forwardedImageType(std::string_view scope, std::string_view node,
                                          std::string_view input, int depth) const {
    // The type of the edge into `input`: what the node on the other end
    // says it is, followed through writers that hand on what they wrote.
    const auto *writer = depth > 0 ? findNode(scope, node) : nullptr;
    if (writer == nullptr) return nullptr;
    const auto inputs = writer->value("inputs", Json::object());
    const auto edge = inputs.is_object() ? inputs.value(std::string{input}, Json{}) : Json{};
    if (!edge.is_object()) return nullptr;
    const auto source_node = text(edge, "node");
    const auto source_port = text(edge, "port");
    const auto *source = findNode(scope, source_node);
    if (source == nullptr) return nullptr;
    if (const auto forwarded = forwardedInputOf(*source, source_port); !forwarded.empty())
        return forwardedImageType(scope, source_node, forwarded, depth - 1);
    const auto typed = [](const Json &type) {
        return type.is_object() && type.contains("semantic") && type.at("semantic").is_string();
    };
    const auto op = text(*source, "op");
    if (op == "graph.input" || op == "graph.instance" || isLoop(*source))
        return imageTypeOf(scope, source_node, source_port, depth - 1);
    const auto parameters = source->value("parameters", Json::object());
    if (!parameters.is_object()) return nullptr;
    const auto ports = parameters.value("ports", Json::object());
    if (ports.is_object())
        if (const auto declared = ports.find(source_port);
            declared != ports.end() && declared->is_object() && declared->contains("type") &&
            typed(declared->at("type")))
            return declared->at("type");
    if (const auto type = parameters.find("type"); type != parameters.end() && typed(*type))
        return *type;
    return nullptr;
}

Json GraphCanvasModel::inverseOps(const Json &ops) const {
    // LP-4c, decision 7. One list can add a node and point a later op at it,
    // or add a definition and then a node in it. Each op is therefore
    // inverted against the document as the ops before it left it: a copy of
    // the document is walked through the list, and every inverse is read off
    // the copy at that point. The inverses run in reverse order; whole nodes
    // and whole definition bodies are restored, so unknown fields and absent
    // values survive Undo.
    Json nodes = nodes_;
    Json definitions = definitions_.is_object() ? definitions_ : Json::object();
    const auto scopeNodesOf = [&](const std::string &scope) -> Json * {
        if (scope.empty()) return &nodes;
        const auto found = definitions.find(scope);
        if (found == definitions.end() || !found->is_object()) return nullptr;
        auto &body = *found;
        if (!body.contains("nodes") || !body.at("nodes").is_array())
            body["nodes"] = Json::array();
        return &body.at("nodes");
    };
    const auto locate = [](Json &scope_nodes, const std::string &id) {
        return std::ranges::find_if(scope_nodes, [&](const Json &value) {
            return value.is_object() && text(value, "id") == id;
        });
    };
    // A definition renamed in the list is still restored under the name the
    // document had before it, which is the name the placement is filed under.
    std::map<std::string, std::string> original_name;
    const auto originalScope = [&](const std::string &scope) {
        const auto found = original_name.find(scope);
        return found == original_name.end() ? scope : found->second;
    };
    std::vector<Json> inverses;
    std::set<std::string> layouts;
    for (const auto &op : ops) {
        const auto kind = op.at("op").get<std::string>();
        const auto scope = op.contains("scope") && op.at("scope").is_string()
                               ? op.at("scope").get<std::string>()
                               : std::string{"root"};
        const auto model_scope = scope == "root" ? std::string{} : scope;
        if (kind == "add_definition") {
            const auto name = op.at("name").get<std::string>();
            inverses.push_back(Json{{"op", "remove_definition"}, {"name", name}});
            auto body = op.value("definition", Json{});
            if (body.is_null()) body = Json{{"nodes", Json::array()}, {"outputs", Json::object()}};
            definitions[name] = std::move(body);
            original_name.erase(name);
            continue;
        }
        if (kind == "remove_definition") {
            const auto name = op.at("name").get<std::string>();
            const auto found = definitions.find(name);
            if (found == definitions.end())
                throw std::logic_error("Graph history definition is missing: " + name);
            // The body goes back whole, placement included: it travels inside.
            inverses.push_back(
                Json{{"op", "add_definition"}, {"name", name}, {"definition", *found}});
            definitions.erase(found);
            continue;
        }
        if (kind == "rename_definition") {
            const auto from = op.at("from").get<std::string>();
            const auto to = op.at("to").get<std::string>();
            const auto found = definitions.find(from);
            if (found == definitions.end())
                throw std::logic_error("Graph history definition is missing: " + from);
            inverses.push_back(Json{{"op", "rename_definition"}, {"from", to}, {"to", from}});
            auto body = std::move(*found);
            definitions.erase(from);
            definitions[to] = std::move(body);
            // The references follow the body in the same op, as the engine
            // moves them: an instance's definition and a loop's branches.
            for (auto &node : nodes) {
                if (!node.is_object()) continue;
                if (text(node, "op") == "graph.instance" && text(node, "definition") == from)
                    node["definition"] = to;
                if (isLoop(node) && node.contains("parameters") &&
                    node.at("parameters").is_object() &&
                    node.at("parameters").contains("branches") &&
                    node.at("parameters").at("branches").is_array())
                    for (auto &branch : node["parameters"]["branches"])
                        if (branch.is_object() && text(branch, "body") == from)
                            branch["body"] = to;
            }
            original_name[to] = originalScope(from);
            original_name.erase(from);
            continue;
        }
        if (kind == "set_layout") {
            layouts.insert(originalScope(model_scope));
            continue;
        }
        if (kind == "set_outputs") {
            // LP-4d2 (DO-1): a scope's outputs, replaced whole; its inverse is
            // the same op carrying what the scope held before. Only a
            // definition's are kept here -- the canvas never sends the
            // root's.
            if (model_scope.empty())
                throw std::logic_error("Graph history cannot invert the root's outputs");
            const auto found = definitions.find(model_scope);
            if (found == definitions.end() || !found->is_object())
                throw std::logic_error("Graph history scope is missing: " + scope);
            const auto previous = found->find("outputs");
            inverses.push_back(Json{{"op", "set_outputs"}, {"scope", scope},
                                    {"outputs", previous != found->end() && previous->is_object()
                                                    ? *previous
                                                    : Json::object()}});
            (*found)["outputs"] = op.at("outputs");
            continue;
        }
        auto *scope_nodes = scopeNodesOf(model_scope);
        if (scope_nodes == nullptr)
            throw std::logic_error("Graph history scope is missing: " + scope);
        if (kind == "add_node") {
            const auto &node = op.at("node");
            inverses.push_back(Json{{"scope", scope}, {"op", "remove_node"},
                                    {"node", node.at("id")}});
            const auto index = op.find("index");
            if (index != op.end() && index->is_number_integer() &&
                index->get<std::int64_t>() >= 0 &&
                index->get<std::size_t>() <= scope_nodes->size())
                scope_nodes->insert(scope_nodes->begin() + index->get<std::ptrdiff_t>(), node);
            else
                scope_nodes->push_back(node);
            continue;
        }
        // replace_node names its node by the node it carries.
        const auto id = kind == "replace_node" ? op.at("node").at("id").get<std::string>()
                                               : op.at("node").get<std::string>();
        const auto found = locate(*scope_nodes, id);
        if (found == scope_nodes->end())
            throw std::logic_error("Graph history node is missing: " + id);
        Json back{{"scope", scope}};
        back["op"] = kind == "remove_node" ? "add_node" : "replace_node";
        back["node"] = *found;
        if (kind == "remove_node")
            back["index"] = std::distance(scope_nodes->begin(), found);
        inverses.push_back(std::move(back));
        // Topology edits discard placements for removed nodes/edges in the
        // same transaction. Restore those only after their endpoints.
        if (kind == "remove_node" || kind == "set_input" || kind == "replace_node")
            layouts.insert(originalScope(model_scope));
        // And the copy moves on, so the next op reads what this one left.
        auto &node = *found;
        if (kind == "remove_node") {
            scope_nodes->erase(found);
        } else if (kind == "replace_node") {
            node = op.at("node");
        } else if (kind == "set_input") {
            const auto port = op.at("port").get<std::string>();
            const auto &source = op.at("source");
            if (source.is_null()) {
                if (node.contains("inputs") && node.at("inputs").is_object()) {
                    node["inputs"].erase(port);
                    if (node["inputs"].empty()) node.erase("inputs");
                }
            } else {
                node["inputs"][port] = source;
            }
        } else if (kind == "set_node_parameters") {
            const auto &parameters = op.at("parameters");
            if (parameters.empty()) node.erase("parameters");
            else node["parameters"] = parameters;
        } else if (kind == "set_node_label") {
            const auto &label = op.at("label");
            if (label.is_string() && label.get_ref<const std::string &>().empty())
                node.erase("label");
            else
                node["label"] = label;
        }
    }
    Json inverse = Json::array();
    for (auto it = inverses.rbegin(); it != inverses.rend(); ++it)
        inverse.push_back(std::move(*it));
    for (const auto &scope : layouts) {
        // A scope the list created has no placement to go back to: the
        // inverse removes it.
        if (!scope.empty() && !definitions_.contains(scope)) continue;
        // A scope with no placement goes back to having none: an empty one
        // would be a member the document did not have.
        const auto layout = scopeLayout(scope);
        inverse.push_back(Json{{"op", "set_layout"},
            {"scope", scope.empty() ? std::string{"root"} : scope},
            {"layout", layout.empty() ? Json{} : layout}});
    }
    return inverse;
}

bool GraphCanvasModel::submitHistory(HistoryAction action, HistoryEntry entry) {
    if (!canEdit()) return false;
    Json ops = action == HistoryAction::Undo ? entry.inverse : entry.forward;
    if (folded_stash_) return refuseFoldedEdit(ops);
    pending_placement_only_ =
        ops.is_array() && !ops.empty() && std::ranges::all_of(ops, [](const Json &op) {
            return op.is_object() && op.value("op", std::string{}) == "set_layout";
        });
    Json params{{"base_source_digest", digest_}, {"ops", std::move(ops)}};
    // GL-1d, decision 3: what the list sends beside its ops goes with it and
    // with its Redo, never with its Undo.
    if (action != HistoryAction::Undo && entry.params.is_object())
        for (const auto &[key, value] : entry.params.items())
            if (!(route_default_unknown_ && key == "material_route_default"))
                params[key] = value;
    pending_history_ = PendingHistory{action, std::move(entry)};
    edit_notice_ = {InspectorNoticeKind::Information, "Saving the graph change..."};
    if (action == HistoryAction::Edit && historySubmitted) historySubmitted();
    request(RequestKind::Submit, "edit_graph_document", std::move(params));
    return true;
}

bool GraphCanvasModel::retryWithoutRouteDefault(const std::string &message) {
    if (!pending_history_ || !pending_history_->entry.params.is_object() ||
        !pending_history_->entry.params.contains("material_route_default") ||
        route_default_unknown_)
        return false;
    // edit_graph_document's own refusal of a field it does not take; the
    // route default's name inside another refusal (a hint) is not it.
    if (message.find("unsupported field") == std::string::npos ||
        message.find("material_route_default") == std::string::npos)
        return false;
    route_default_unknown_ = true;
    auto retry = std::move(*pending_history_);
    discardPendingHistory();
    retry.entry.params.erase("material_route_default");
    return submitHistory(retry.action, std::move(retry.entry));
}

void GraphCanvasModel::forgetHistory() {
    clearHistory();
}

void GraphCanvasModel::discardPendingHistory() {
    if (!pending_history_) return;
    const auto pending = std::exchange(pending_history_, std::nullopt);
    const auto move = pending->action == HistoryAction::Edit ? HistoryMove::Push
                    : pending->action == HistoryAction::Undo ? HistoryMove::Undo : HistoryMove::Redo;
    if (historyMoved) historyMoved(pending->entry.serial, move, false);
}

void GraphCanvasModel::clearHistory() {
    undo_.clear();
    redo_.clear();
    discardPendingHistory();
    history_digest_.clear();
    render_restore_digest_.clear();
    keep_history_through_digest_ = false;
    if (historyCleared) historyCleared();
}

void GraphCanvasModel::commitHistory(const Json &response) {
    const auto digest = response.value("source_digest", Json::object())
                            .value("hex", std::string{});
    if (!pending_history_ || digest.empty()) {
        clearHistory();
        return;
    }
    auto pending = std::move(*pending_history_);
    pending_history_.reset();
    // Q-19: a scope the committed ops renamed, for the trail (an Undo sends
    // the inverse, which renames it back).
    for (const auto &op : pending.action == HistoryAction::Undo ? pending.entry.inverse
                                                                  : pending.entry.forward)
        if (op.is_object() && text(op, "op") == "rename_definition")
            committed_renames_[text(op, "from")] = text(op, "to");
    if (pending.action == HistoryAction::Edit) pending.entry.serial = next_history_serial_++;
    const auto serial = pending.entry.serial;
    if (pending.action == HistoryAction::Undo) {
        undo_.pop_back();
        redo_.push_back(std::move(pending.entry));
    } else {
        if (pending.action == HistoryAction::Redo) redo_.pop_back();
        else redo_.clear();
        undo_.push_back(std::move(pending.entry));
        if (undo_.size() > historyCapacity) undo_.erase(undo_.begin());
    }
    history_digest_ = digest;
    const auto move = pending.action == HistoryAction::Edit ? HistoryMove::Push
                    : pending.action == HistoryAction::Undo ? HistoryMove::Undo : HistoryMove::Redo;
    if (historyMoved) historyMoved(serial, move, true);
}

bool GraphCanvasModel::undo() {
    return canUndo() && submitHistory(HistoryAction::Undo, undo_.back());
}

bool GraphCanvasModel::redo() {
    return canRedo() && submitHistory(HistoryAction::Redo, redo_.back());
}

std::string GraphCanvasModel::nodeOperation(std::string_view scope, std::string_view id) const {
    const auto *node = findNode(scope, id);
    return node == nullptr ? std::string{} : text(*node, "op");
}

Json GraphCanvasModel::nodeParameters(std::string_view id) const {
    return nodeParameters(currentScope(), id);
}

Json GraphCanvasModel::nodeParameters(std::string_view scope, std::string_view id) const {
    const auto *node = findNode(scope, id);
    if (node == nullptr) return Json::object();
    const auto found = node->find("parameters");
    if (found == node->end() || !found->is_object()) return Json::object();
    return *found;
}

bool GraphCanvasModel::canConnect(std::string_view from_node, std::string_view from_port,
                                  std::string_view to_node, std::string_view to_port) const {
    return canConnect(currentScope(), from_node, from_port, to_node, to_port);
}

bool GraphCanvasModel::canConnect(std::string_view scope, std::string_view from_node,
                                  std::string_view from_port, std::string_view to_node,
                                  std::string_view to_port) const {
    if (from_node.empty() || to_node.empty() || from_node == to_node) return false;
    return canConnectAmong(nodes(scope), from_node, from_port, to_node, to_port);
}

bool GraphCanvasModel::canConnectAmong(const std::vector<GraphCanvasNode> &drawn,
                                       std::string_view from_node, std::string_view from_port,
                                       std::string_view to_node, std::string_view to_port) {
    if (from_node.empty() || to_node.empty() || from_node == to_node) return false;
    const auto producer = std::ranges::find(drawn, std::string{from_node}, &GraphCanvasNode::id);
    const auto consumer = std::ranges::find(drawn, std::string{to_node}, &GraphCanvasNode::id);
    if (producer == drawn.end() || consumer == drawn.end()) return false;
    const auto output = std::ranges::find(producer->outputs, std::string{from_port},
                                          &GraphCanvasOutput::port);
    const auto input = std::ranges::find(consumer->inputs, std::string{to_port},
                                         &GraphCanvasPort::port);
    if (output == producer->outputs.end() || input == consumer->inputs.end()) return false;
    // A port that declares no accepted type takes anything, and a producer the
    // catalog never typed cannot be refused on a type it does not have.
    if (output->type.empty() || input->type.empty()) return true;
    return output->type == input->type;
}

bool GraphCanvasModel::connectInput(const std::string &node, const std::string &port,
                                    const std::string &source_node,
                                    const std::string &source_port) {
    return connectInput(currentScope(), node, port, source_node, source_port);
}

bool GraphCanvasModel::connectInput(std::string_view scope, const std::string &node,
                                    const std::string &port, const std::string &source_node,
                                    const std::string &source_port) {
    if (!canEdit()) return false;
    if (!canConnect(scope, source_node, source_port, node, port)) return false;
    // EL-5s, decision 1: a compute node's empty socket (a sampled image its
    // shader declares) is declared as the line lands, in the same edit.
    if (const auto open = computeOpenSockets(scope, node); std::ranges::find(open, port) != open.end()) {
        auto parameters = nodeParameters(scope, node);
        parameters["ports"][port] =
            Json{{"kind", "image"},
                 {"type", imageTypeOf(scope, source_node, source_port, 8)},
                 {"access", "read"},
                 {"binding", "sampled"},
                 {"sampling", Json{{"filter", "nearest"}, {"address", "clamp_to_edge"}}}};
        return submitOps(Json::array(
            {Json{{"op", "set_node_parameters"}, {"scope", scopeToken(scope)}, {"node", node},
                  {"parameters", std::move(parameters)}},
             Json{{"op", "set_input"}, {"scope", scopeToken(scope)}, {"node", node}, {"port", port},
                  {"source", Json{{"node", source_node}, {"port", source_port}}}}}));
    }
    Json ops = Json::array();
    // UX-8, decision 1: a buffer fed by a value is one runtime float
    // (graphpartition: "scalar-connected buffer requires one runtime float").
    // Its size goes with the line; another element is refused here, by name.
    if (const auto *target = findNode(scope, node);
        port == "value" && target != nullptr &&
        (text(*target, "op") == "render.buffer" || text(*target, "op") == "render.attribute")) {
        auto parameters = nodeParameters(scope, node);
        const auto element = parameters.is_object() ? text(parameters, "element") : std::string{};
        if (element != "float") {
            edit_notice_ = {InspectorNoticeKind::Error,
                            node + " holds " + (element.empty() ? std::string{"no element"} : element) +
                                ": a value's line writes one float. For two to four numbers, "
                                "write them with a render.scalar."};
            return false;
        }
        if (text(*target, "op") == "render.buffer" && parameters.value("size", Json{}) != Json(4)) {
            parameters["size"] = 4;
            ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", scopeToken(scope)},
                               {"node", node}, {"parameters", std::move(parameters)}});
        }
    }
    ops.push_back(Json{
        {"op", "set_input"}, {"scope", scopeToken(scope)}, {"node", node}, {"port", port},
        {"source", Json{{"node", source_node}, {"port", source_port}}}});
    return submitOps(std::move(ops));
}

bool GraphCanvasModel::disconnectInput(const std::string &node, const std::string &port) {
    return disconnectInput(currentScope(), node, port);
}

bool GraphCanvasModel::disconnectInput(std::string_view scope, const std::string &node,
                                       const std::string &port) {
    if (!canEdit() || findNode(scope, node) == nullptr) return false;
    // Q-19, decision 2: a numbered port is its line; taking the line away
    // takes the port, and the ones after it move down a number.
    if (auto ops = removeInputPortOps(scope, node, port); !ops.empty())
        return submitOps(std::move(ops));
    // UX-8, decision 2: so is a pass's named buffer port -- a port with no
    // line is not one the engine binds.
    if (const auto buffers = bufferPorts(scope, node);
        isGraphicsPass(text(*findNode(scope, node), "op")) &&
        std::ranges::find(buffers, port) != buffers.end()) {
        auto parameters = nodeParameters(scope, node);
        parameters["ports"].erase(port);
        return submitOps(Json::array(
            {Json{{"op", "set_input"}, {"scope", scopeToken(scope)}, {"node", node}, {"port", port},
                  {"source", nullptr}},
             Json{{"op", "set_node_parameters"}, {"scope", scopeToken(scope)}, {"node", node},
                  {"parameters", std::move(parameters)}}}));
    }
    return submitOps(Json::array({Json{
        {"op", "set_input"}, {"scope", scopeToken(scope)}, {"node", node}, {"port", port},
        {"source", nullptr}}}));
}

bool GraphCanvasModel::setNodeParameter(const std::string &node, const std::string &key,
                                        const Json &value) {
    return setNodeParameter(currentScope(), node, key, value);
}

bool GraphCanvasModel::setNodeParameter(std::string_view scope, const std::string &node,
                                        const std::string &key, const Json &value) {
    if (!canEdit() || findNode(scope, node) == nullptr) return false;
    if (key.empty()) return false;
    // set_node_parameters replaces the object, so the whole of it is read back
    // and sent again with this one key changed.
    auto parameters = nodeParameters(scope, node);
    parameters[key] = value;
    return submitOps(Json::array({Json{
        {"op", "set_node_parameters"}, {"scope", scopeToken(scope)}, {"node", node},
        {"parameters", std::move(parameters)}}}));
}

bool GraphCanvasModel::removeNode(const std::string &node) {
    return removeNode(currentScope(), node);
}

bool GraphCanvasModel::removeNode(std::string_view scope, const std::string &node) {
    if (!canEdit() || findNode(scope, node) == nullptr) return false;
    return submitOps(Json::array({Json{
        {"op", "remove_node"}, {"scope", scopeToken(scope)}, {"node", node}}}));
}

nlohmann::json GraphCanvasModel::scopeLayout(std::string_view scope) const {
    if (scope.empty()) return layout_.is_object() ? layout_ : Json::object();
    const auto found = definitions_.find(std::string{scope});
    if (found == definitions_.end() || !found->is_object()) return Json::object();
    auto layout = found->value("layout", Json::object());
    return layout.is_object() ? layout : Json::object();
}

bool GraphCanvasModel::setLayout(const std::string &scope, const Json &layout) {
    if (!canEdit()) return false;
    return submitOps(Json::array({Json{
        {"op", "set_layout"},
        {"scope", scope.empty() ? std::string{"root"} : scope},
        {"layout", layout}}}));
}

bool GraphCanvasModel::setNodeLabel(const std::string &node, const std::string &label) {
    return setNodeLabel(currentScope(), node, label);
}

bool GraphCanvasModel::setNodeLabel(std::string_view scope, const std::string &node,
                                    const std::string &label) {
    if (!canEdit() || findNode(scope, node) == nullptr) return false;
    return submitOps(Json::array({Json{
        {"op", "set_node_label"}, {"scope", scopeToken(scope)}, {"node", node},
        {"label", label}}}));
}

Json GraphCanvasModel::definitionDefaults(std::string_view definition) const {
    Json parameters = Json::object();
    const auto body = definitions_.find(std::string{definition});
    if (body == definitions_.end()) return parameters;
    for (const auto &node : body->value("nodes", Json::array())) {
        if (!node.is_object() || text(node, "op") != "graph.input") continue;
        const auto id = text(node, "id");
        if (id.empty()) continue;
        const auto declared = node.find("default");
        parameters[id] = declared != node.end() && declared->is_number() ? *declared : Json(0.0);
    }
    return parameters;
}

std::string GraphCanvasModel::suggestNodeId(std::string_view op) const {
    return suggestNodeId(currentScope(), op);
}

std::string GraphCanvasModel::suggestNodeId(std::string_view scope, std::string_view op) const {
    std::string base{op};
    // The op name is the honest starting point, but a node id is an identifier
    // the document points at: the dots go.
    std::ranges::replace(base, '.', '_');
    if (base.empty()) base = "node";
    const auto taken = [&](const std::string &candidate) {
        return findNode(scope, candidate) != nullptr;
    };
    if (!taken(base)) return base;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = base + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return base;
}

Json GraphCanvasModel::addNodeOps(const std::string &op, const std::string &definition) const {
    return addNodeOpsIn(currentScope(), op, definition);
}

Json GraphCanvasModel::addNodeOpsIn(std::string_view in_scope, const std::string &op,
                                    const std::string &definition, const Json &seed) const {
    Json ops = Json::array();
    if (op.empty()) return ops;
    const auto scope = scopeToken(in_scope);
    const bool root = in_scope.empty();
    if (!root && !definitions_.contains(std::string{in_scope})) return ops;
    auto id = suggestNodeId(in_scope, op);
    Json node{{"id", id}, {"op", op}};
    if (op == "graph.input") {
        if (root) return Json::array();
        // A boundary port every instance can already stand on: without a value
        // to fall back to, the instances that exist stop expanding.
        node["default"] = 0.0;
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
        return ops;
    }
    // LP-4c: a loop over an existing body. A new body is addLoopOps's second
    // argument, which the palette passes itself.
    if (op == "graph.loop") return root ? addLoopOps(definition) : Json::array();
    if (op == "graph.instance") {
        if (!root || definition.empty() || !definitions_.contains(definition))
            return Json::array();
        node["definition"] = definition;
        node["parameters"] = definitionDefaults(definition);
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
        return ops;
    }
    // UX-3, decision 1: the render nodes whose ports follow their
    // parameters arrive with parameters the engine takes, in one list.
    if (isPaletteSeededRender(op)) {
        if (operation(op) == nullptr || drawBodyScope(in_scope)) return Json::array();
        const auto seedText = [&](const char *key) {
            return seed.is_object() ? text(seed, key) : std::string{};
        };
        const auto add = [&](Json added) {
            ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(added)}});
        };
        // An id beside the scope's others: `base`, else base_2, base_3...
        const auto freeId = [&](const std::string &base) {
            if (findNode(in_scope, base) == nullptr) return base;
            for (int suffix = 2; suffix < 10000; ++suffix) {
                auto candidate = base + "_" + std::to_string(suffix);
                if (findNode(in_scope, candidate) == nullptr) return candidate;
            }
            return base;
        };
        if (op == "render.target_ref") {
            const auto name = seedText("name");
            const auto targets = declaredRenderTargets();
            const auto target = std::ranges::find(targets, name, &GraphCanvasDeclaredTarget::name);
            if (target == targets.end()) return Json::array();
            node["id"] = freeId(name + "_ref");
            node["parameters"] = Json{{"name", name}, {"type", target->type}};
            node["outputs"] = Json::array({"resource"});
            add(std::move(node));
            return ops;
        }
        if (op == "render.image") {
            // UX-2's defaults: the size of the screen (or of the loop's view
            // family), scene colour, sixteen bits a channel.
            Json parameters{{"type", sceneColorType()}, {"format", "R16G16B16A16_SFLOAT"},
                            {"extent_scale", 1.0},
                            {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
            const auto family = viewAxisFamilyOf(in_scope);
            if (const auto extent = family.empty() ? std::nullopt : viewFamilyImageExtent(family))
                for (const auto &[key, value] : extent->items()) parameters[key] = value;
            node["parameters"] = std::move(parameters);
            node["outputs"] = Json::array({"resource"});
            add(std::move(node));
            return ops;
        }
        if (op == "render.buffer" || op == "render.attribute") {
            const bool attribute = op == "render.attribute";
            auto element = seedText("element");
            if (element.empty()) element = "float";
            if (std::ranges::find(bufferElements(), element) == bufferElements().end())
                return Json::array();
            auto domain = seedText("domain");
            if (attribute && domain.empty()) domain = attributeDomains().front();
            if (attribute &&
                std::ranges::find(attributeDomains(), domain) == attributeDomains().end())
                return Json::array();
            // An index attribute is read as uint (graphmodule's bufferType).
            if (attribute && domain == "index") element = "uint";
            std::int64_t count = 1;
            if (seed.is_object())
                if (const auto found = seed.find("count");
                    found != seed.end() && found->is_number_integer())
                    count = found->get<std::int64_t>();
            if (count <= 0) return Json::array();
            Json parameters{{"element", element}};
            // An attribute's size follows its count; a buffer's is its bytes.
            if (attribute) {
                parameters["domain"] = domain;
                parameters["count"] = count;
            } else {
                parameters["size"] = static_cast<std::uint64_t>(count) * bufferElementSize(element);
            }
            node["parameters"] = std::move(parameters);
            node["outputs"] = Json::array({"resource"});
            add(std::move(node));
            return ops;
        }
        if (op == "render.compute") {
            // EL-5s, decision 1: a project shader whose interface the engine
            // has read arrives with its ports, images and lines.
            auto stem = seedText("shader");
            if (stem.empty() && seedText("ray_tracing").starts_with("project://"))
                stem = seedText("ray_tracing");
            if (const auto known = stem.empty() ? std::nullopt : shaderInterface(stem);
                known && known->ready() &&
                (std::ranges::find(compute_shader_choices_, stem) != compute_shader_choices_.end() ||
                 std::ranges::find(project_ray_tracing_choices_, stem) !=
                     project_ray_tracing_choices_.end()))
                return computeShaderOps(in_scope, id, stem);
            if (seedText("ray_tracing").starts_with("project://")) return Json::array();
        }
        if (op == "render.compute" && !seedText("ray_tracing").empty()) {
            // UX-5d, decision 2: a task tracing with one of the engine's ray
            // tracing pipelines. Its ports are the shader's resource ports
            // (RS-5 binds them by name): an image it writes gets an image of
            // its own beside it, and the rays are as many as that image's
            // pixels; an image it samples is read from the image of the
            // scope whose name says the same (world_position from a
            // G-buffer's worldpos), from the pass that writes it.
            const auto pipelines = rayTracingPipelines();
            const auto pipeline = std::ranges::find(pipelines, seedText("ray_tracing"),
                                                    &GraphCanvasRayTracingPipeline::reference);
            if (pipeline == pipelines.end()) return Json::array();
            const auto key = [](std::string name) {
                std::ranges::transform(name, name.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                std::erase(name, '_');
                return name;
            };
            const auto sources = templateSources(in_scope, id);
            std::vector<std::string> taken;
            const auto matchSource = [&](const std::string &port) -> const GraphCanvasTemplateSource * {
                const auto wanted = key(port);
                std::vector<const GraphCanvasTemplateSource *> found;
                for (const auto &source : sources) {
                    auto tail = source.resource;
                    if (const auto split = tail.rfind("__"); split != std::string::npos)
                        tail = tail.substr(split + 2);
                    else if (tail.starts_with("gbuffer_"))
                        tail = tail.substr(8);
                    const auto have = key(tail);
                    if (have.size() < 3 || wanted.size() < 3) continue;
                    if ((have == wanted || wanted.starts_with(have) || have.starts_with(wanted)) &&
                        std::ranges::find(taken, source.resource) == taken.end())
                        found.push_back(&source);
                }
                return found.size() == 1 ? found.front() : nullptr;
            };
            Json ports = Json::object();
            Json inputs = Json::object();
            Json outputs = Json::array();
            std::string rays_from;
            for (const auto &[port, type] : pipeline->ports) {
                if (type == "storage_image") {
                    const auto image = freeId(id + "__" + port);
                    // EL-4c: the format the shader writes it in, when the
                    // engine could read it.
                    const auto format = pipeline->formats.contains(port)
                                            ? pipeline->formats.at(port)
                                            : std::string{"R16G16B16A16_SFLOAT"};
                    add(Json{{"id", image}, {"op", "render.image"},
                             {"parameters", Json{{"type", sceneColorType()},
                                                 {"format", format},
                                                 {"extent_scale", 1.0},
                                                 {"usage", Json::array({"STORAGE", "SAMPLED"})}}},
                             {"outputs", Json::array({"resource"})}});
                    ports[port] = Json{{"kind", "image"}, {"type", sceneColorType()},
                                       {"access", "write"}, {"binding", "storage"}};
                    inputs[port] = Json{{"node", image}, {"port", "resource"}};
                    outputs.push_back(port);
                    if (rays_from.empty()) rays_from = port;
                } else if (type == "sampled_image") {
                    Json read_type = sceneColorType();
                    if (const auto *source = matchSource(port)) {
                        taken.push_back(source->resource);
                        if (const auto *image = findNode(in_scope, source->resource);
                            image != nullptr && image->is_object())
                            if (const auto declared = image->value("parameters", Json::object())
                                                          .value("type", Json{});
                                declared.is_object())
                                read_type = declared;
                        inputs[port] = Json{{"node", source->node}, {"port", source->port}};
                    }
                    ports[port] = Json{{"kind", "image"}, {"type", std::move(read_type)},
                                       {"access", "read"}, {"binding", "sampled"},
                                       {"sampling", Json{{"filter", "nearest"},
                                                         {"address", "clamp_to_edge"}}}};
                }
                // A buffer port has nothing here to bind; the engine names it.
            }
            if (rays_from.empty()) return Json::array();
            node["parameters"] = Json{
                {"ray_tracing", Json{{"raygen", pipeline->raygen},
                                     {"miss", pipeline->miss},
                                     {"closesthit", pipeline->closesthit}}},
                {"dispatch", Json{{"rays_from", Json{{"port", rays_from}}}}},
                {"ports", std::move(ports)}};
            node["inputs"] = std::move(inputs);
            node["outputs"] = std::move(outputs);
            add(std::move(node));
            return ops;
        }
        if (op == "render.compute") {
            // The Task Editor's new task: a shader of the project's, one
            // group, nothing bound yet.
            if (compute_shader_choices_.empty()) return Json::array();
            if (const auto picked = seedText("shader"); !picked.empty()) {
                if (std::ranges::find(compute_shader_choices_, picked) == compute_shader_choices_.end())
                    return Json::array();
                node["parameters"] = Json{{"shader", picked},
                                          {"dispatch", Json{{"groups", Json::array({1, 1, 1})}}},
                                          {"ports", Json::object()}};
                node["outputs"] = Json::array();
                add(std::move(node));
                return ops;
            }
            const auto preferred = std::ranges::find(compute_shader_choices_,
                                                     std::string{"project://shaders/compute"});
            node["parameters"] = Json{
                {"shader", preferred != compute_shader_choices_.end() ? *preferred
                                                                      : compute_shader_choices_.front()},
                {"dispatch", Json{{"groups", Json::array({1, 1, 1})}}},
                {"ports", Json::object()}};
            node["outputs"] = Json::array();
            add(std::move(node));
            return ops;
        }
        if (op == "render.shadow_depth") {
            // As a material pass's: the depth image it draws, beside it.
            const auto image = id + "__depth";
            add(Json{{"id", image}, {"op", "render.image"},
                     {"parameters",
                      Json{{"type", Json{{"semantic", "pelican.render.depth@1"},
                                         {"arguments", Json{{"representation", "device"},
                                                            {"space", "projection"}}}}},
                           {"format", "D32_SFLOAT"}, {"extent_scale", 1.0},
                           {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})}}},
                     {"outputs", Json::array({"resource"})}});
            node["parameters"] = Json::object();
            node["inputs"] = Json{{"depth", Json{{"node", image}, {"port", "resource"}}}};
            node["outputs"] = Json::array({"depth"});
            add(std::move(node));
            return ops;
        }
        if (op == "render.snapshot_copy") {
            // The engine refuses a copy that names no pass it copies after.
            const auto after = seedText("snapshot_after");
            if (after.empty()) return Json::array();
            Json inputs = Json::object();
            snapshotCopySeedOps(in_scope, scope, id, after, ops, inputs);
            node["parameters"] = Json{{"snapshot_after", after}};
            node["inputs"] = std::move(inputs);
            node["outputs"] = Json::array({"destination"});
            add(std::move(node));
            return ops;
        }
    }
    if (op == kFullscreenTemplate || op == "render.raster") {
        // UX-3, decision 1: a raster pass arrives as a fullscreen one does,
        // drawing the engine's passthrough over three vertices; without
        // those shaders in the list it is not offered.
        if (op == "render.raster" && (!rasterTemplateAvailable() || drawBodyScope(in_scope)))
            return Json::array();
        // LP-4c. A fullscreen pass is not a name alone: its ports, its shader
        // and the family it runs in are its parameters. It arrives drawing
        // with the engine's passthrough into an image of its own -- the
        // smallest pass that is a whole document -- and the image is added
        // in the same list, as a required input's constant is.
        if (operation(op) == nullptr) return Json::array();
        // UX-12b, decision 2: a pass a feature is inserted after is named as
        // the feature names it (the seed's id), when that id is free.
        if (seed.is_object())
            if (const auto wanted = text(seed, "id");
                !wanted.empty() && findNode(in_scope, wanted) == nullptr) {
                id = wanted;
                node["id"] = id;
            }
        const Json type{{"semantic", "pelican.render.color_signal@1"},
                        {"arguments", Json{{"reference", "scene"}, {"transfer", "linear"},
                                           {"range", "extended"}}}};
        const auto image = id + "__color";
        // UX-6a, decisions 3 and 4: scene-linear colour brighter than 1 needs
        // a float image (an eight-bit one clips the light a bloom extracts),
        // and no TRANSFER_SRC: the engine reads that flag as "read from
        // outside the frame plan" and never aliases such an image. A pass
        // made here reads nothing yet and nothing reads it, present
        // included, so no new image is the last one before present.
        Json image_parameters{{"type", type}, {"format", "R16G16B16A16_SFLOAT"},
                              {"extent_scale", 1.0},
                              {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
        // LP-4f2 (VF-1): in the body of a loop over a view axis, the pass
        // draws in that loop's family, whose passes all draw at one size --
        // the frame plan's family_extent (VF-2). The image is made at it, and the
        // pass names the family (the View field's "Each element" then narrows
        // it to one view a copy).
        const auto family = viewAxisFamilyOf(in_scope);
        const auto extent = family.empty() ? std::nullopt : viewFamilyImageExtent(family);
        if (extent)
            for (const auto &[key, value] : extent->items()) image_parameters[key] = value;
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope},
            {"node", Json{{"id", image}, {"op", "render.image"},
                          {"parameters", std::move(image_parameters)},
                          {"outputs", Json::array({"resource"})}}}});
        // RU-1 (LP-1e): no pass_set. A graph pass that names none joins the
        // first family of the composed frame, which is where the scene is
        // drawn; naming main_render put it nowhere in a pipeline without one.
        node["parameters"] = Json{
            {"shader", Json{{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
            {"ports", Json{{"color", Json{{"kind", "image"}, {"type", type}, {"access", "write"},
                                          {"attachment", "color"}, {"location", 0}}}}}};
        // A raster pass draws what its `draw` says: the three vertices of
        // the screen-covering triangle engine://fullscreen makes.
        if (op == "render.raster") node["parameters"]["draw"] = Json{{"vertex_count", 3}};
        if (!family.empty()) {
            node["parameters"]["view_family"] = family;
            // A fixed size is not the screen's: the pass is resolved on its
            // own, as the engine's cube and shadow passes are.
            if (extent && extent->contains("width"))
                node["parameters"]["resolution_domain"] = "independent";
        }
        node["inputs"] = Json{{"color", Json{{"node", image}, {"port", "resource"}}}};
        node["outputs"] = Json::array({"color"});
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
        // RU-1 (LP-1e): the loop running this body is not given a place. A
        // loop with none expands after the scene passes, before the first
        // anchor, which is where post_main used to put it; the Loop fields'
        // Place still moves it.
        return ops;
    }
    if (op == kMaterialPass) {
        // LP-4e. A material pass is a contract and its attachments: it
        // arrives with the contract the palette's combo chose (the
        // `definition` argument), writing images of its own -- one per
        // socket, and a depth -- added in the same list, as a fullscreen
        // pass's is.
        if (!root || operation(op) == nullptr) return Json::array();
        const auto contract = definition.empty() ? materialPassContracts().front() : definition;
        if (std::ranges::find(materialPassContracts(), contract) == materialPassContracts().end())
            return Json::array();
        // GL-1d, decision 1: and it arrives with its draws, as
        // decompose_material_pass writes them (and as GL-1's folded pass
        // shows them): an else-only draw_loop `<pass>_draws` over the pass's
        // materials, whose body `<pass>_draw` draws each one. The id is one
        // whose loop and body names are free as well.
        const auto free = [&](const std::string &candidate) {
            return findNode(in_scope, candidate) == nullptr &&
                   findNode(in_scope, candidate + "_draws") == nullptr &&
                   !definitions_.contains(candidate + "_draw");
        };
        std::string base{op};
        std::ranges::replace(base, '.', '_');
        for (int suffix = 2; !free(id) && suffix < 10000; ++suffix)
            id = base + "_" + std::to_string(suffix);
        node["id"] = id;
        const auto loop = id + "_draws";
        const auto body = id + "_draw";
        ops.push_back(Json{{"op", "add_definition"}, {"name", body},
            {"definition", Json{{"nodes", Json::array({Json{{"id", "step"},
                                                            {"op", "draw.material"},
                                                            {"parameters", Json::object()},
                                                            {"outputs", Json::array({"draws"})}}})},
                                {"outputs", Json{{"draws", Json{{"node", "step"},
                                                                {"port", "draws"}}}}}}}});
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope},
            {"node", Json{{"id", loop}, {"op", "graph.loop"},
                          {"parameters",
                           Json{{"over", Json{{"draw_items", Json{{"contract", contract}}}}},
                                {"tier", "draw_loop"},
                                {"order", Json{{"by", "phase_default"}}},
                                {"branches", Json::array({Json{{"body", body}}})}}},
                          {"outputs", Json::array({"draws"})}}}});
        Json parameters{{"material_contract", contract}, {"pass_set", "main_render"}};
        const Json color_type{{"semantic", "pelican.render.color_signal@1"},
                              {"arguments", Json{{"reference", "scene"}, {"transfer", "linear"},
                                                 {"range", "extended"}}}};
        const auto image = [&](const std::string &socket, const Json &type,
                               const MaterialPassSlot &slot) {
            const auto image_id = id + "__" + socket;
            ops.push_back(Json{{"op", "add_node"}, {"scope", scope},
                {"node", Json{{"id", image_id}, {"op", "render.image"},
                              {"parameters", Json{{"type", type}, {"format", slot.format},
                                                  {"extent_scale", 1.0},
                                                  {"usage", slot.usage}}},
                              {"outputs", Json::array({"resource"})}}}});
            return Json{{"node", image_id}, {"port", "resource"}};
        };
        // MC-1s: the formats are the contract's table, the one the Frame
        // Plan's material pass form makes its targets with. A material pass
        // without a material_outputs schema is checked against the physical
        // contract the live materials were registered with, and those follow
        // the pipeline's HDR switch: without "hdr" among the enabled features
        // (a Blank project) a G-buffer's albedo and emissive are B8G8R8A8_SRGB.
        // Any other format changes the contract, and the engine restarts
        // (material_physical_contract_changed).
        std::vector<MaterialPassSlot> colors;
        std::optional<MaterialPassSlot> depth;
        for (auto &slot : materialPassSlots(contract, materialPassHdr(render_authoring_)))
            if (slot.depth) depth = std::move(slot);
            else colors.push_back(std::move(slot));
        const auto sockets = materialPassColorSockets(parameters);
        if (!depth || sockets.size() != colors.size()) return Json::array();
        Json inputs = Json::object();
        for (std::size_t index = 0; index < sockets.size(); ++index)
            inputs[sockets[index]] = image(sockets[index], color_type, colors[index]);
        inputs["depth"] = image("depth",
                                Json{{"semantic", "pelican.render.depth@1"},
                                     {"arguments", Json{{"representation", "device"},
                                                        {"space", "projection"}}}},
                                *depth);
        inputs["draws"] = Json{{"node", loop}, {"port", "draws"}};
        node["parameters"] = std::move(parameters);
        node["inputs"] = std::move(inputs);
        node["outputs"] = Json::array();
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
        return ops;
    }
    if (isDrawStep(op)) {
        // LP-4e, decision 4. A draw step stands in a draw_loop body only. It
        // arrives at the head of the body's list -- drawn first, the step
        // that makes `draws` still last -- so the body keeps one list.
        if (root || !drawBodyScope(in_scope) || operation(op) == nullptr)
            return Json::array();
        Json parameters = Json::object();
        // Something to draw differently, so the step is one the engine
        // takes: the proposed variant, or both faces.
        if (op == "draw.variant") {
            // UX-3, decision 5: a variant some material has, when the engine
            // lists them and the pass names none.
            auto variant = suggestDrawVariant(in_scope);
            if (const auto known = materialVariants(); variant == "variant" && !known.empty())
                variant = known.front();
            parameters["variant"] = variant;
        }
        if (op == "draw.override") parameters["render_state"] = Json{{"cull", "none"}};
        node["parameters"] = std::move(parameters);
        node["outputs"] = Json::array({"draws"});
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", node}});
        // The head: from the step `draws` names, back along `after`.
        const auto &body = definitions_.at(std::string{in_scope});
        const auto outputs = body.value("outputs", Json::object());
        const auto draws = outputs.is_object() ? outputs.find("draws") : outputs.end();
        std::string head = draws != outputs.end() && draws->is_object() ? text(*draws, "node") : "";
        std::set<std::string> seen;
        while (!head.empty() && seen.insert(head).second) {
            const auto *step = findNode(in_scope, head);
            if (step == nullptr) break;
            const auto after = step->value("inputs", Json::object()).value("after", Json{});
            if (!after.is_object() || text(after, "node").empty()) break;
            head = text(after, "node");
        }
        const auto *head_node = head.empty() ? nullptr : findNode(in_scope, head);
        if (head_node != nullptr && isDrawStep(text(*head_node, "op")))
            ops.push_back(Json{{"op", "set_input"}, {"scope", scope}, {"node", head},
                               {"port", "after"},
                               {"source", Json{{"node", id}, {"port", "draws"}}}});
        return ops;
    }
    const auto *contract = operation(op);
    if (contract == nullptr || text(*contract, "ports") != "declared") return Json::array();
    auto outputs = Json::array();
    for (const auto &output : contract->value("outputs", Json::array())) {
        const auto name = text(output, "port");
        if (!name.empty()) outputs.push_back(name);
    }
    node["outputs"] = std::move(outputs);
    Json inputs = Json::object();
    for (const auto &input : contract->value("inputs", Json::array())) {
        const auto port = text(input, "port");
        if (port.empty() || !input.value("required", false)) continue;
        // A required input with nothing on it is not a document the engine
        // loads, so the node arrives standing on its own constant -- which is
        // also the number the row's pill then edits.
        if (semanticTypeName(input, "accepted_type") != kGraphInputType) continue;
        // Q-11. A voice reads its sound along `time`; a constant there would
        // hold it on its first sample. It arrives on the scene clock instead
        // -- the scope's own scene_time when it has one, a new one otherwise
        // -- and so starts on its owner's first frame (AU-3). The clock and
        // the node go out as one edit, so one Undo takes both back.
        // UX-8, decision 3: a curve and a playback likewise -- a curve read
        // at a constant time is one number, and a playback whose elapsed
        // stands still never plays.
        if ((isAudioPlayback(op) && port == "time") || (op == "value.curve" && port == "time") ||
            (op == "value.playback" && port == "elapsed")) {
            auto clock = sceneTimeNode(in_scope);
            if (clock.empty()) {
                clock = suggestNodeId(in_scope, "scene_time");
                ops.push_back(Json{{"op", "add_node"}, {"scope", scope},
                    {"node", Json{{"id", clock}, {"op", "value.input"},
                                  {"parameters", Json{{"name", "scene_time"}}},
                                  {"outputs", Json::array({"value"})}}}});
            }
            inputs[port] = Json{{"node", clock}, {"port", "value"}};
            continue;
        }
        const auto seed = id + "__" + port;
        // UX-8, decision 7: a sound's volume or pitch starts where the voice
        // plays as it is (1), not at silence.
        const double start = op == "audio.volume" || op == "audio.pitch" ? 1.0 : 0.0;
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope},
            {"node", Json{{"id", seed}, {"op", "value.constant"},
                          {"parameters", Json{{"value", start}}},
                          {"outputs", Json::array({"value"})}}}});
        inputs[port] = Json{{"node", seed}, {"port", "value"}};
    }
    // UX-8, decision 7: the edges the palette's dialog chose (a sound sink's
    // voice), beside the ones seeded here.
    if (seed.is_object())
        if (const auto given = seed.find("inputs"); given != seed.end() && given->is_object())
            for (const auto &[port, edge] : given->items())
                if (edge.is_object()) inputs[port] = edge;
    if (!inputs.empty()) node["inputs"] = std::move(inputs);
    // UX-8, decision 3: a curve arrives as a ramp from 0 to 1 over its first
    // second, which its Keys table edits.
    if (op == "value.curve")
        node["parameters"]["keys"] =
            Json::array({Json{{"time", 0.0}, {"value", 0.0}}, Json{{"time", 1.0}, {"value", 1.0}}});
    // Q-8. A ui.* sink without a node path is not a document the engine
    // loads either ("ui sink requires a node path"). It arrives writing the
    // first node the project's UI document offers for it; the details panel
    // lists the rest. With no UI document read, the key stays out and the
    // engine's refusal names what is missing.
    if (const auto key = uiNodeParameter(op); !key.empty()) {
        const auto choices = uiNodeChoices(op);
        if (!choices.empty())
            node["parameters"] = Json{{std::string{key}, choices.front()}};
    }
    // Q-10. audio.playback has no port for the sound it plays, and without
    // one the engine cannot lower it. It arrives on the project's first
    // sound; the `asset` field lists the rest. With none found, the key stays
    // out and the engine's refusal names what is missing. Its optional ports
    // (events, position, the time map) are left for the user to connect.
    if (isAudioPlayback(op) && !audio_assets_.empty())
        node["parameters"] = Json{{"asset", audio_assets_.front()}};
    // UX-7, decision 5: what the palette's small dialog asked for -- the
    // parameters an op cannot be placed without (requiredParameters).
    if (seed.is_object())
        if (const auto given = seed.find("parameters"); given != seed.end() && given->is_object())
            for (const auto &[key, value] : given->items()) node["parameters"][key] = value;
    // UX-13b, decision 2 (engine CT-5): a render.scalar joins its buffer
    // after the sinks already on it, and the buffer is made one they can all
    // write (runtime_f32, as large as they are) in the same edit.
    Json fix = Json::array();
    if (op == "render.scalar" && node.contains("parameters") && node.at("parameters").is_object()) {
        auto &parameters = node.at("parameters");
        const auto target = text(parameters, "target");
        if (!target.empty()) {
            const auto before = scalarTargetBytes(in_scope, target);
            if (before > 0 && !parameters.contains("offset"))
                parameters["offset"] = static_cast<std::int64_t>(before);
            const auto value = parameters.find("value");
            const std::uint64_t own =
                (value != parameters.end() && value->is_array() ? value->size() : 1) * sizeof(float);
            fix = scalarTargetFixOps(in_scope, target, before + own);
        }
    }
    ops.push_back(Json{{"op", "add_node"}, {"scope", scope}, {"node", std::move(node)}});
    for (auto &each : fix) ops.push_back(std::move(each));
    return ops;
}

bool GraphCanvasModel::addNode(const std::string &op, const std::string &definition) {
    return addNodeIn(currentScope(), op, definition);
}

bool GraphCanvasModel::addNodeIn(std::string_view scope, const std::string &op,
                                 const std::string &definition, const Json &seed) {
    if (!canEdit()) return false;
    auto ops = addNodeOpsIn(scope, op, definition, seed);
    if (ops.empty()) return false;
    auto params = addNodeParams(scope, op, definition);
    if (params.empty()) return submitOps(std::move(ops));
    if (folded_stash_) return refuseFoldedEdit(ops);
    auto inverse = inverseOps(ops);
    return submitHistory(HistoryAction::Edit,
                         {std::move(ops), std::move(inverse), std::move(params)});
}

Json GraphCanvasModel::addNodeParams(std::string_view scope, const std::string &op,
                                     const std::string &definition) const {
    Json params = Json::object();
    if (op != kMaterialPass || !scope.empty() || route_default_unknown_) return params;
    const auto contract = definition.empty() ? materialPassContracts().front() : definition;
    if (materialRouteOffersForwardDefault(contract, pipelineHasDeferredMaterialPass()))
        params["material_route_default"] = "forward";
    return params;
}

bool GraphCanvasModel::pipelineHasDeferredMaterialPass() const {
    // The plan's material passes, by the contract material_routing names
    // (a pass it does not name is not known to be deferred)...
    for (const auto &pass : plan_passes_)
        if (pass.material && !pass.contract.empty() &&
            materialContractTakesDeferredRoute(pass.contract))
            return true;
    // ...and the root's own, which a plan read before them does not list.
    const auto &nodes = folded_stash_ ? folded_stash_->nodes : nodes_;
    for (const auto &node : nodes) {
        if (!node.is_object() || text(node, "op") != kMaterialPass) continue;
        const auto parameters = node.find("parameters");
        const auto contract = parameters != node.end() && parameters->is_object()
                                  ? text(*parameters, "material_contract")
                                  : std::string{};
        if (materialContractTakesDeferredRoute(contract)) return true;
    }
    return false;
}

// --- LP-4c: graph.loop.

Json GraphCanvasModel::loopParameters(std::string_view id) const {
    const auto *node = findNode({}, id);
    if (node == nullptr || !isLoop(*node)) return Json::object();
    const auto found = node->find("parameters");
    if (found == node->end() || !found->is_object()) return Json::object();
    return *found;
}

bool GraphCanvasModel::setLoopParameters(const std::string &id, Json parameters) {
    if (!canEdit()) return false;
    const auto *node = findNode({}, id);
    if (node == nullptr || !isLoop(*node)) return false;
    if (node->value("parameters", Json::object()) == parameters) return false;
    // Loops stand in the root document only, so the op names the root
    // whatever scope the canvas is showing.
    return submitOps(Json::array({Json{
        {"op", "set_node_parameters"}, {"scope", "root"}, {"node", id},
        {"parameters", std::move(parameters)}}}));
}

std::optional<GraphCanvasLoop> GraphCanvasModel::loop(std::string_view id) const {
    const auto *node = findNode({}, id);
    if (node == nullptr || !isLoop(*node)) return std::nullopt;
    GraphCanvasLoop loop;
    loop.id = std::string{id};
    const auto parameters = loopParameters(id);
    const auto over = parameters.value("over", Json::object());
    // LP-4e: over the draws of one material pass.
    if (over.is_object())
        if (const auto draw_items = over.find("draw_items");
            draw_items != over.end() && draw_items->is_object()) {
            loop.draw_items = true;
            loop.contract = text(*draw_items, "contract");
        }
    const auto range = over.is_object() ? over.value("range", Json::object()) : Json::object();
    const auto count = range.is_object() ? range.find("count") : range.end();
    if (range.is_object() && count != range.end()) {
        if (count->is_number_integer()) {
            loop.count = count->get<std::int64_t>();
            loop.known_count = loop.count;
        } else if (count->is_object() && count->contains("$expr")) {
            loop.count_expression = true;
            loop.count_text = text(*count, "$expr");
        }
    }
    // LP-4f1: over one view axis; its length is the frame plan's.
    if (over.is_object())
        if (const auto view_axis = over.find("view_axis");
            view_axis != over.end() && view_axis->is_object()) {
            loop.view_family = text(*view_axis, "family");
            loop.view_axis = text(*view_axis, "axis");
            if (const auto *axes = familyAxes(loop.view_family))
                if (const auto axis = std::ranges::find(*axes, loop.view_axis, &FramePlanViewAxis::axis);
                    axis != axes->end()) {
                    loop.view_axis_info = *axis;
                    if (axis->length && !axis->runtime)
                        loop.known_count = static_cast<std::int64_t>(*axis->length);
                }
        }
    const auto order = parameters.find("order");
    loop.descending = order != parameters.end() && order->is_object() &&
                      text(*order, "direction") == "descending";
    const auto limit = parameters.find("limit");
    if (limit != parameters.end() && limit->is_number_integer())
        loop.limit = limit->get<std::int64_t>();
    loop.tier = text(parameters, "tier");
    const auto where = parameters.find("where");
    if (loop.draw_items && where != parameters.end() && where->is_object()) {
        if (const auto tags = where->find("tags"); tags != where->end())
            if (auto filter = readTagFilter(*tags)) loop.where_tags = std::move(*filter);
    } else if (where != parameters.end() && where->is_object() && where->size() == 1) {
        const auto first = where->begin();
        if (first.key() == "element_in" || first.key() == "element_not_in")
            if (auto values = integerList(first.value())) {
                loop.where = first.key();
                loop.where_values = std::move(*values);
            }
    }
    loop.anchor = text(parameters, "canonical_anchor");
    if (const auto insert = parameters.find("insert");
        insert != parameters.end() && insert->is_object() && insert->size() == 1)
        loop.insert = insert->begin().key() + " " +
                      (insert->begin()->is_string() ? insert->begin()->get<std::string>()
                                                    : insert->begin()->dump());
    for (const auto &branch : parameters.value("branches", Json::array()))
        loop.branches.push_back(readLoopBranch(branch));
    const auto inputs = node->value("inputs", Json::object());
    for (const auto &pair : parameters.value("carry", Json::array())) {
        if (!pair.is_object()) continue;
        GraphCanvasLoopCarry carry{text(pair, "from"), text(pair, "to"), false};
        carry.to_unbound = !inputs.is_object() || !inputs.contains(carry.to);
        loop.carry.push_back(std::move(carry));
    }
    for (const auto &output : node->value("outputs", Json::array()))
        if (output.is_string()) loop.outputs.push_back(output.get<std::string>());
    for (const auto &branch : loop.branches)
        if (!branch.body.empty() && definitions_.contains(branch.body)) {
            loop.body = branch.body;
            loop.body_inputs = definitionInputs(definitions_.at(branch.body));
            loop.body_outputs = definitionOutputs(definitions_.at(branch.body));
            break;
        }
    for (const auto &branch : loop.branches) {
        const auto *nodes = branch.body.empty() ? nullptr : scopeNodes(branch.body);
        if (nodes == nullptr) continue;
        for (const auto &body_node : *nodes) {
            const auto op = text(body_node, "op");
            loop.body_draws = loop.body_draws || op == "render.fullscreen" || op == "render.raster";
        }
    }
    // LP-4e: the material pass the loop's draws feed.
    for (const auto &candidate : nodes_) {
        if (!candidate.is_object() || text(candidate, "op") != kMaterialPass) continue;
        const auto draws = candidate.value("inputs", Json::object()).value("draws", Json{});
        if (draws.is_object() && text(draws, "node") == id) {
            loop.pass = text(candidate, "id");
            break;
        }
    }
    if (const auto *report = loopReport(id)) {
        loop.reported = true;
        loop.reported_tier = text(*report, "tier");
        loop.reported_count = report->value("count", std::int64_t{0});
        if (loop.pass.empty()) loop.pass = text(*report, "pass");
        const auto elements = report->value("elements", Json::array());
        for (const auto &element : elements) {
            if (!element.is_object()) continue;
            const auto found = element.find("element");
            if (found == element.end()) continue;
            // A draw_loop's element is a material's matching key.
            if (found->is_string()) loop.materials.push_back(found->get<std::string>());
            const auto branch = element.value("branch", std::int64_t{-1});
            if (branch < 0 || static_cast<std::size_t>(branch) >= loop.branches.size()) continue;
            auto &taken = loop.branches[static_cast<std::size_t>(branch)];
            if (found->is_string())
                taken.matched_materials.push_back(found->get<std::string>());
            else if (found->is_number_integer())
                taken.matched.push_back(found->get<std::int64_t>());
        }
        loop.reported_total = report->contains("total") && report->at("total").is_number_unsigned()
                                  ? report->at("total").get<std::size_t>()
                                  : elements.size();
        for (const auto &branch : report->value("unmatched_branches", Json::array()))
            if (branch.is_number_integer() && branch.get<std::int64_t>() >= 0 &&
                static_cast<std::size_t>(branch.get<std::int64_t>()) < loop.branches.size())
                loop.branches[static_cast<std::size_t>(branch.get<std::int64_t>())].unmatched = true;
    }
    return loop;
}

std::string GraphCanvasModel::suggestDefinitionName(std::string_view base) const {
    std::string name{base.empty() ? std::string_view{"loop_body"} : base};
    std::ranges::replace(name, '.', '_');
    std::ranges::replace(name, '/', '_');
    const auto taken = [&](const std::string &candidate) {
        return candidate == "root" || definitions_.contains(candidate);
    };
    if (!taken(name)) return name;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = name + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return name;
}

Json GraphCanvasModel::addLoopOps(const std::string &body,
                                  const std::string &new_definition) const {
    Json ops = Json::array();
    // A loop stands in the root document only (graph.loop_nested@1).
    if (!currentScope().empty()) return ops;
    std::string runs = body;
    auto outputs = Json::array();
    if (!new_definition.empty()) {
        if (new_definition == "root" || new_definition.find('/') != std::string::npos ||
            definitions_.contains(new_definition))
            return Json::array();
        ops.push_back(Json{{"op", "add_definition"}, {"name", new_definition}});
        runs = new_definition;
    } else {
        if (body.empty() || !definitions_.contains(body)) return Json::array();
        for (const auto &output : definitionOutputs(definitions_.at(body))) outputs.push_back(output);
    }
    Json node{{"id", suggestNodeId("loop")},
              {"op", "graph.loop"},
              {"parameters",
               Json{{"over", Json{{"range", Json{{"count", 1}}}}},
                    {"branches", Json::array({Json{{"body", runs}}})}}},
              {"outputs", std::move(outputs)}};
    ops.push_back(Json{{"op", "add_node"}, {"scope", "root"}, {"node", std::move(node)}});
    return ops;
}

bool GraphCanvasModel::addLoop(const std::string &body, const std::string &new_definition) {
    if (!canEdit()) return false;
    auto ops = addLoopOps(body, new_definition);
    if (ops.empty()) return false;
    return submitOps(std::move(ops));
}

bool GraphCanvasModel::setLoopCount(const std::string &id, std::int64_t count) {
    auto parameters = loopParameters(id);
    if (count < 0) return false;
    parameters["over"] = Json{{"range", Json{{"count", count}}}};
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopCountExpression(const std::string &id,
                                              const std::string &expression) {
    if (expression.empty()) return false;
    auto parameters = loopParameters(id);
    parameters["over"] = Json{{"range", Json{{"count", Json{{"$expr", expression}}}}}};
    return setLoopParameters(id, std::move(parameters));
}

// --- LP-4f1: view axes.

namespace {

// `view_axes` as get_frame_plan writes it, read leniently: an entry the
// field cannot use is skipped rather than failing the whole plan (the
// Frame Plan tab is where a malformed plan is reported).
std::vector<FramePlanViewAxis> planViewAxes(const Json &node) {
    std::vector<FramePlanViewAxis> axes;
    const auto found = node.find("view_axes");
    if (found == node.end() || !found->is_array()) return axes;
    for (const auto &entry : *found) {
        if (!entry.is_object()) continue;
        FramePlanViewAxis axis;
        axis.axis = text(entry, "axis");
        if (axis.axis.empty()) continue;
        if (const auto length = entry.find("length");
            length != entry.end() && length->is_number_unsigned())
            axis.length = length->get<std::uint64_t>();
        axis.runtime = entry.value("runtime", false);
        if (const auto labels = entry.find("labels"); labels != entry.end() && labels->is_array())
            for (const auto &label : *labels)
                if (label.is_string()) axis.labels.push_back(label.get<std::string>());
        axes.push_back(std::move(axis));
    }
    return axes;
}

bool isElementExpression(const Json &value) {
    return value.is_object() && value.size() == 1 && text(value, "$expr") == "element";
}

} // namespace

void GraphCanvasModel::readPlanViews(const Json &plan) {
    view_families_.clear();
    plan_node_views_.clear();
    plan_targets_.clear();
    family_extents_.clear();
    if (const auto nodes = plan.find("nodes"); nodes != plan.end() && nodes->is_array())
        for (const auto &node : *nodes) {
            if (!node.is_object()) continue;
            const auto family = text(node, "view_family");
            // LP-4f3 (VF-2): the size the engine holds the family's passes
            // to, the same on every node of it, read as the Frame Plan tab
            // reads it (Q-20). A node whose family_extent Studio cannot read
            // is passed over, as a malformed axis is: the Frame Plan tab is
            // where it is named, and here the image falls back to the
            // screen's size.
            if (!family.empty() && !family_extents_.contains(family)) {
                try {
                    if (auto extent = framePlanFamilyExtent(node, "get_frame_plan node"))
                        family_extents_.emplace(family, std::move(*extent));
                } catch (const std::exception &) {
                }
            }
            auto axes = planViewAxes(node);
            if (family.empty() || axes.empty()) continue;
            if (const auto name = text(node, "name"); !name.empty())
                plan_node_views_[name] = GraphCanvasViewFamily{family, axes};
            // The axes are the family's, the same on every node of it
            // (LP-Va2): the first node that lists them names them.
            if (std::ranges::none_of(view_families_, [&](const auto &known) {
                    return known.family == family;
                }))
                view_families_.push_back(GraphCanvasViewFamily{family, std::move(axes)});
        }
    if (const auto resources = plan.find("resources");
        resources != plan.end() && resources->is_array())
        for (const auto &resource : *resources)
            if (resource.is_object() && text(resource, "kind") == "render_target" &&
                !text(resource, "name").empty())
                plan_targets_.push_back(text(resource, "name"));
    std::ranges::sort(plan_targets_);
    plan_targets_.erase(std::unique(plan_targets_.begin(), plan_targets_.end()),
                        plan_targets_.end());
}

std::string GraphCanvasModel::viewAxisFamilyOf(std::string_view scope) const {
    if (scope.empty()) return {};
    for (const auto &candidate : nodes_) {
        if (!isLoop(candidate)) continue;
        const auto bodies = loopBranchBodies(candidate);
        if (std::ranges::find(bodies, scope) == bodies.end()) continue;
        const auto parameters = candidate.value("parameters", Json::object());
        const auto over = parameters.is_object() ? parameters.value("over", Json::object())
                                                 : Json::object();
        const auto view_axis = over.is_object() ? over.value("view_axis", Json::object())
                                                : Json::object();
        if (const auto family = view_axis.is_object() ? text(view_axis, "family") : std::string{};
            !family.empty())
            return family;
    }
    return {};
}

bool GraphCanvasModel::isLoopBodyScope(std::string_view scope) const {
    if (scope.empty()) return false;
    return std::ranges::any_of(nodes_, [&](const Json &candidate) {
        if (!isLoop(candidate)) return false;
        const auto bodies = loopBranchBodies(candidate);
        return std::ranges::find(bodies, scope) != bodies.end();
    });
}

GraphCanvasLoopElementRead GraphCanvasModel::loopElementRead(std::string_view scope,
                                                             std::string_view node) const {
    if (node.empty()) return GraphCanvasLoopElementRead::none;
    bool reads = false;
    bool copied = false;
    const auto note = [&](const GraphCanvasPlanPass &pass) {
        if (!pass.reads_loop_index) return;
        reads = true;
        if (!(pass.origin ? pass.origin->loop : pass.loop).empty()) copied = true;
    };
    if (scope.empty()) {
        for (const auto &name : planNodesOf(node))
            if (const auto *pass = planPass(name)) note(*pass);
    } else {
        // A body node: the passes whose origin is it, in that definition
        // (an engine that predates origin: loop_origin's body node).
        for (const auto &pass : plan_passes_) {
            const bool made = pass.origin
                                  ? pass.origin->definition == scope && pass.origin->node == node
                                  : !pass.loop.empty() && pass.loop_node == node;
            if (made) note(pass);
        }
    }
    if (!reads) return GraphCanvasLoopElementRead::none;
    return copied ? GraphCanvasLoopElementRead::in_loop : GraphCanvasLoopElementRead::outside_loop;
}

std::optional<Json> GraphCanvasModel::viewFamilyImageExtent(std::string_view family) const {
    const auto found = family_extents_.find(family);
    if (found == family_extents_.end()) return std::nullopt;
    const auto &extent = found->second;
    // A fixed size is the same size; one relative to the output, the same
    // scale. Either way the family's layers (a cube's six faces are six views
    // of one image).
    Json image = extent.size
                     ? Json{{"width", (*extent.size)[0]}, {"height", (*extent.size)[1]}}
                     : Json{{"extent_scale", extent.output_scale}};
    if (extent.layers > 1) image["layers"] = extent.layers;
    return image;
}

const std::vector<FramePlanViewAxis> *GraphCanvasModel::familyAxes(std::string_view family) const {
    const auto found = std::ranges::find(view_families_, family, &GraphCanvasViewFamily::family);
    return found == view_families_.end() ? nullptr : &found->axes;
}

bool GraphCanvasModel::takesViewSelect(std::string_view op) noexcept {
    return op == "render.fullscreen" || op == "render.raster" || op == "render.compute" ||
           op == "render.material_pass" || op == "render.shadow_depth" ||
           op == "render.snapshot_copy";
}

bool GraphCanvasModel::setLoopViewAxis(const std::string &id, const std::string &family,
                                       const std::string &axis) {
    if (family.empty() || axis.empty()) return false;
    auto parameters = loopParameters(id);
    parameters["over"] = Json{{"view_axis", Json{{"family", family}, {"axis", axis}}}};
    return setLoopParameters(id, std::move(parameters));
}

std::string GraphCanvasModel::loopDrawItemsContract(std::string_view id) const {
    const auto *loop = findNode({}, id);
    if (loop == nullptr || !isLoop(*loop)) return {};
    for (const auto &candidate : nodes_) {
        if (!candidate.is_object() || text(candidate, "op") != kMaterialPass) continue;
        const auto inputs = candidate.find("inputs");
        if (inputs == candidate.end() || !inputs->is_object()) continue;
        const auto draws = inputs->find("draws");
        if (draws == inputs->end() || !draws->is_object() || text(*draws, "node") != id) continue;
        const auto parameters = candidate.find("parameters");
        return parameters != candidate.end() && parameters->is_object()
                   ? text(*parameters, "material_contract")
                   : std::string{};
    }
    return {};
}

bool GraphCanvasModel::setLoopDrawItems(const std::string &id) {
    const auto contract = loopDrawItemsContract(id);
    if (contract.empty()) return false;
    auto parameters = loopParameters(id);
    parameters["over"] = Json{{"draw_items", Json{{"contract", contract}}}};
    parameters["tier"] = "draw_loop";
    // A draw_loop keeps the phase's order, runs every material and hands
    // nothing on, and runs inside its pass: no limit, carry, element `where`
    // or place of its own.
    parameters["order"] = Json{{"by", "phase_default"}};
    for (const auto *key : {"limit", "carry", "where", "insert", "canonical_anchor"})
        parameters.erase(key);
    return setLoopParameters(id, std::move(parameters));
}

std::optional<GraphCanvasViewField> GraphCanvasModel::viewField(std::string_view scope,
                                                                std::string_view id) const {
    const auto *node = findNode(scope, id);
    if (node == nullptr || !takesViewSelect(text(*node, "op"))) return std::nullopt;
    const auto parameters = node->value("parameters", Json::object());
    const auto own = parameters.is_object() ? text(parameters, "view_family") : std::string{};
    GraphCanvasViewField field;
    const auto select = parameters.is_object() ? parameters.value("view_select", Json::object())
                                               : Json::object();
    if (select.is_object())
        for (const auto &[axis, value] : select.items()) {
            if (value.is_number_unsigned())
                field.view_select.push_back(
                    FramePlanViewSelectAxis{axis, value.get<std::uint64_t>()});
            else if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
                field.view_select.push_back(FramePlanViewSelectAxis{
                    axis, static_cast<std::uint64_t>(value.get<std::int64_t>())});
        }
    // In a body a loop over a view axis runs: that loop's family, and
    // "Each element" on its axis -- unless the node names another family.
    if (!scope.empty())
        for (const auto &candidate : nodes_) {
            if (!isLoop(candidate)) continue;
            const auto bodies = loopBranchBodies(candidate);
            if (std::ranges::find(bodies, scope) == bodies.end()) continue;
            const auto loop_parameters = candidate.value("parameters", Json::object());
            const auto over = loop_parameters.is_object()
                                  ? loop_parameters.value("over", Json::object())
                                  : Json::object();
            const auto view_axis = over.is_object() ? over.value("view_axis", Json::object())
                                                    : Json::object();
            const auto family = view_axis.is_object() ? text(view_axis, "family") : std::string{};
            if (family.empty() || (!own.empty() && own != family)) continue;
            field.family = family;
            field.loop = text(candidate, "id");
            field.element_axis = text(view_axis, "axis");
            break;
        }
    if (field.family.empty() && scope.empty())
        if (const auto planned = plan_node_views_.find(id); planned != plan_node_views_.end()) {
            field.family = planned->second.family;
            field.axes = planned->second.axes;
        }
    if (field.family.empty()) field.family = own;
    if (field.family.empty()) return std::nullopt;
    if (field.axes.empty())
        if (const auto *axes = familyAxes(field.family)) field.axes = *axes;
    if (field.axes.empty()) return std::nullopt;
    field.writes_family = own.empty();
    if (!field.element_axis.empty() && select.is_object())
        if (const auto chosen = select.find(field.element_axis); chosen != select.end())
            field.element_chosen = isElementExpression(*chosen);
    return field;
}

bool GraphCanvasModel::setViewSelect(std::string_view scope, const std::string &node,
                                     const Json &view_select, const std::string &family) {
    if (!canEdit() || findNode(scope, node) == nullptr) return false;
    if (!view_select.is_object()) return false;
    auto parameters = nodeParameters(scope, node);
    const auto before = parameters;
    if (view_select.empty()) {
        parameters.erase("view_select");
    } else {
        parameters["view_select"] = view_select;
        if (!family.empty() && text(parameters, "view_family").empty())
            parameters["view_family"] = family;
    }
    if (parameters == before) return false;
    return submitOps(Json::array({Json{
        {"op", "set_node_parameters"}, {"scope", scopeToken(scope)}, {"node", node},
        {"parameters", std::move(parameters)}}}));
}

bool GraphCanvasModel::setLoopDescending(const std::string &id, bool descending) {
    auto parameters = loopParameters(id);
    // Ascending is what an absent order means, so it is written by leaving
    // the member out rather than by spelling the default.
    if (descending)
        parameters["order"] = Json{{"by", "element"}, {"direction", "descending"}};
    else
        parameters.erase("order");
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopLimit(const std::string &id, std::optional<std::int64_t> limit) {
    auto parameters = loopParameters(id);
    if (limit) {
        if (*limit < 0) return false;
        parameters["limit"] = *limit;
    } else {
        parameters.erase("limit");
    }
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopTier(const std::string &id, const std::string &tier) {
    auto parameters = loopParameters(id);
    if (tier.empty()) parameters.erase("tier");
    else if (tier == "expand" || tier == "draw_loop") parameters["tier"] = tier;
    else return false;
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopWhere(const std::string &id, const std::string &kind,
                                    const std::vector<std::int64_t> &values) {
    auto parameters = loopParameters(id);
    if (kind.empty()) parameters.erase("where");
    else if (kind == "element_in" || kind == "element_not_in")
        parameters["where"] = Json{{kind, values}};
    else return false;
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopCarry(
    const std::string &id, const std::vector<std::pair<std::string, std::string>> &carry) {
    auto parameters = loopParameters(id);
    if (carry.empty()) {
        parameters.erase("carry");
    } else {
        auto pairs = Json::array();
        for (const auto &[from, to] : carry) {
            if (from.empty() || to.empty()) return false;
            pairs.push_back(Json{{"from", from}, {"to", to}});
        }
        parameters["carry"] = std::move(pairs);
    }
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopOutputs(const std::string &id,
                                      const std::vector<std::string> &outputs) {
    if (!canEdit()) return false;
    const auto *node = findNode({}, id);
    if (node == nullptr || !isLoop(*node)) return false;
    auto replaced = *node;
    replaced["outputs"] = outputs;
    if (replaced == *node) return false;
    // No op sets a node's outputs on its own: the node goes back whole, which
    // is also exactly what its Undo sends.
    return submitOps(Json::array({Json{
        {"op", "replace_node"}, {"scope", "root"}, {"node", std::move(replaced)}}}));
}

const std::vector<std::string> &GraphCanvasModel::loopAnchors() {
    // The engine's canonical anchors (featurecompose.cpp), in frame order.
    static const std::vector<std::string> anchors{
        "sprite", "post_main", "tonemap", "post_ldr",
        "pelican_ui", "debug_draw", "debug_text", "imgui"};
    return anchors;
}

bool GraphCanvasModel::setLoopAnchor(const std::string &id, const std::string &anchor) {
    auto parameters = loopParameters(id);
    if (!anchor.empty() && std::ranges::find(loopAnchors(), anchor) == loopAnchors().end())
        return false;
    // One place at a time: an anchor and an insert exclude each other.
    parameters.erase("insert");
    if (anchor.empty()) parameters.erase("canonical_anchor");
    else parameters["canonical_anchor"] = anchor;
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::addLoopBranch(const std::string &id) {
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || branches.empty()) return false;
    // A new branch goes in front of the else and runs the else's body, so
    // the list it arrives in is one the engine takes: the same boundary, and
    // a condition (element 0) that is an integer the table can edit.
    const auto &last = branches.back();
    Json added{{"when", Json{{"element", 0}}}};
    // LP-4e: a draw_loop's elements are materials. The new branch catches
    // the first material the report names; before a report, the materials
    // that have a variant -- a condition the engine takes as it is.
    if (const auto *node = findNode({}, id); node != nullptr && isDrawItemsLoop(*node)) {
        const auto materials = loopMaterials(id);
        added["when"] = materials.empty() ? Json{{"has_variant", true}}
                                          : Json{{"material", materials.front()}};
    }
    added["body"] = last.is_object() && last.contains("body") ? last.at("body") : Json{};
    // UX-1, decision 4: a branch that ran the else's own body could not be
    // told from the else -- its tab showed the same body, and an edit there
    // changed both. It runs a copy of that body, a new definition under a
    // name Studio proposes (as New definition… does), in the same list:
    // one Undo takes both back.
    Json ops = Json::array();
    if (const auto &body = added.at("body"); body.is_string() &&
                                             definitions_.contains(body.get<std::string>())) {
        const auto name = suggestDefinitionName(id + "_body");
        ops.push_back(Json{{"op", "add_definition"},
                           {"name", name},
                           {"definition", definitions_.at(body.get<std::string>())}});
        added["body"] = name;
    }
    branches.insert(branches.end() - 1, std::move(added));
    parameters["branches"] = std::move(branches);
    if (ops.empty()) return setLoopParameters(id, std::move(parameters));
    if (!canEdit()) return false;
    ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", "root"}, {"node", id},
                       {"parameters", std::move(parameters)}});
    return submitOps(std::move(ops));
}

bool GraphCanvasModel::removeLoopBranch(const std::string &id, std::size_t branch) {
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    // The else is the last row, and it is never removed.
    if (!branches.is_array() || branch + 1 >= branches.size()) return false;
    branches.erase(branches.begin() + static_cast<std::ptrdiff_t>(branch));
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::moveLoopBranch(const std::string &id, std::size_t branch, int delta) {
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || branch + 1 >= branches.size() || delta == 0) return false;
    const auto target = static_cast<std::int64_t>(branch) + delta;
    // Only among the conditional rows: the else stays last.
    if (target < 0 || static_cast<std::size_t>(target) + 1 >= branches.size()) return false;
    std::swap(branches[branch], branches[static_cast<std::size_t>(target)]);
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopBranchWhen(const std::string &id, std::size_t branch,
                                         const std::string &when,
                                         const std::vector<std::int64_t> &values) {
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || branch + 1 >= branches.size()) return false;
    auto &row = branches[branch];
    if (!row.is_object()) return false;
    if (when == "element") {
        if (values.size() != 1) return false;
        row["when"] = Json{{"element", values.front()}};
    } else if (when == "element_in" || when == "element_not_in") {
        row["when"] = Json{{when, values}};
    } else {
        return false;
    }
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopBranchBody(const std::string &id, std::size_t branch,
                                         const std::optional<std::string> &body) {
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || branch >= branches.size() || !branches[branch].is_object())
        return false;
    if (body && !definitions_.contains(*body)) return false;
    branches[branch]["body"] = body ? Json(*body) : Json{};
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::newLoopBranchDefinition(const std::string &id, std::size_t branch,
                                               const std::string &name) {
    if (!canEdit()) return false;
    const auto shape = loop(id);
    if (!shape || branch >= shape->branches.size()) return false;
    if (name.empty() || name == "root" || name.find('/') != std::string::npos ||
        definitions_.contains(name))
        return false;
    // The loop's boundary, empty inside: the first body's graph.input nodes
    // as they are declared, and its outputs. An output a boundary port feeds
    // is kept feeding it; an output some other node makes cannot be written
    // without that node, so the body is copied whole instead -- a branch
    // whose boundary differs is refused (graph.loop_branch_interface@1).
    Json definition{{"nodes", Json::array()}, {"outputs", Json::object()}};
    if (!shape->body.empty()) {
        const auto &source = definitions_.at(shape->body);
        bool whole = false;
        for (const auto &node : source.value("nodes", Json::array()))
            if (node.is_object() && text(node, "op") == "graph.input")
                definition["nodes"].push_back(node);
        // items() borrows its container, so the object outlives the loop.
        const auto source_outputs = source.value("outputs", Json::object());
        for (const auto &[port, endpoint] : source_outputs.items()) {
            const auto *producer =
                endpoint.is_object() ? findNode(shape->body, text(endpoint, "node")) : nullptr;
            if (producer == nullptr || text(*producer, "op") != "graph.input") {
                whole = true;
                break;
            }
            definition["outputs"][port] = endpoint;
        }
        if (whole) {
            definition = source;
            definition.erase("layout");
        }
    }
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || !branches[branch].is_object()) return false;
    branches[branch]["body"] = name;
    parameters["branches"] = std::move(branches);
    return submitOps(Json::array({
        Json{{"op", "add_definition"}, {"name", name}, {"definition", std::move(definition)}},
        Json{{"op", "set_node_parameters"}, {"scope", "root"}, {"node", id},
             {"parameters", std::move(parameters)}}}));
}

std::vector<std::string> GraphCanvasModel::loopBodyChoices(std::string_view id) const {
    std::vector<std::string> choices;
    const auto shape = loop(id);
    if (!shape) return choices;
    const auto sorted = [](std::vector<std::string> values) {
        std::ranges::sort(values);
        return values;
    };
    const auto inputs = sorted(shape->body_inputs);
    const auto outputs = sorted(shape->body_outputs);
    for (const auto &[name, definition] : definitions_.items()) {
        // With no body yet, any definition sets the boundary.
        if (!shape->body.empty() && (sorted(definitionInputs(definition)) != inputs ||
                                     sorted(definitionOutputs(definition)) != outputs))
            continue;
        choices.push_back(name);
    }
    return choices;
}

bool GraphCanvasModel::submitEdit(Json ops) { return submitOps(std::move(ops)); }

bool GraphCanvasModel::renameDefinition(const std::string &from, const std::string &to) {
    if (!canEdit() || !definitions_.contains(from) || from == to) return false;
    // Q-19, decision 4: the node id rule and "root" are checked before
    // anything is sent. The engine moves every instance's and every loop
    // branch's name in the same op (visitDefinitionReferences), so the one op
    // is the whole gesture.
    if (auto refusal = definitionNameRefusal(from, to); !refusal.empty()) {
        edit_notice_ = {InspectorNoticeKind::Error, std::move(refusal)};
        return false;
    }
    return submitOps(Json::array({Json{
        {"op", "rename_definition"}, {"from", from}, {"to", to}}}));
}

bool GraphCanvasModel::removeDefinition(const std::string &name) {
    if (!canEdit() || !definitions_.contains(name)) return false;
    // Decision 4: what still runs it is said, and nothing is sent.
    if (auto refusal = definitionRemoveRefusal(name); !refusal.empty()) {
        edit_notice_ = {InspectorNoticeKind::Error, std::move(refusal)};
        return false;
    }
    return submitOps(Json::array({Json{{"op", "remove_definition"}, {"name", name}}}));
}

std::string GraphCanvasModel::definitionNameRefusal(std::string_view from,
                                                    std::string_view to) const {
    if (to == from) return {};
    const std::string name{to};
    if (name.empty()) return "A definition needs a name.";
    if (name.find('/') != std::string::npos)
        return "A definition's name cannot hold a slash: " + name + ".";
    if (name == "root")
        return "A definition cannot be named root: that name is the root document's.";
    if (definitions_.contains(name)) return "A definition named " + name + " already exists.";
    return {};
}

std::vector<std::string> GraphCanvasModel::definitionReferrers(std::string_view definition) const {
    std::vector<std::string> referrers;
    // Instances and loops stand in the root document only (the engine refuses
    // either in a body), so the root is where anything that runs it is.
    for (const auto &node : nodes_) {
        if (!node.is_object()) continue;
        const bool runs =
            (text(node, "op") == "graph.instance" && text(node, "definition") == definition) ||
            (isLoop(node) && runsBody(node, definition));
        const auto id = text(node, "id");
        if (runs && std::ranges::find(referrers, id) == referrers.end()) referrers.push_back(id);
    }
    return referrers;
}

std::string GraphCanvasModel::definitionRemoveRefusal(std::string_view name) const {
    const auto referrers = definitionReferrers(name);
    if (referrers.empty()) return {};
    std::string joined;
    for (const auto &referrer : referrers) {
        if (!joined.empty()) joined += ", ";
        joined += referrer;
    }
    return "Definition " + std::string{name} + " is used by " + joined +
           ": make them run another definition first.";
}

std::vector<std::pair<std::string, std::string>>
GraphCanvasModel::perIterationSources(std::string_view receiver) const {
    std::vector<std::pair<std::string, std::string>> sources;
    for (const auto &node : nodes_) {
        if (!isLoop(node)) continue;
        const auto id = text(node, "id");
        // A loop reading its own `p[]` would be a cycle.
        if (id == receiver) continue;
        for (const auto &output : node.value("outputs", Json::array()))
            if (output.is_string() && output.get<std::string>().ends_with("[]"))
                sources.emplace_back(id, output.get<std::string>());
    }
    return sources;
}

bool GraphCanvasModel::connectPerIteration(const std::string &node, const std::string &port,
                                           const std::string &source_node,
                                           const std::string &source_port,
                                           const std::string &expression) {
    if (!canEdit() || expression.empty()) return false;
    const auto *receiver = findNode({}, node);
    if (receiver == nullptr || !isLoop(*receiver)) return false;
    const auto sources = perIterationSources(node);
    if (std::ranges::find(sources, std::pair{source_node, source_port}) == sources.end())
        return false;
    // `element` in the expression is the receiving loop's element: each of
    // its elements reads that element of the other loop's `p[]`.
    return submitOps(Json::array({Json{
        {"op", "set_input"}, {"scope", "root"}, {"node", node}, {"port", port},
        {"source", Json{{"node", source_node}, {"port", source_port},
                        {"element", Json{{"$expr", expression}}}}}}}));
}

namespace {

// LP-4d2: the definitions a loop's branches run, each once, in branch order.
// A branch that runs nothing, or names a definition the document does not
// hold, adds none.
std::vector<std::string> distinctLoopBodies(const Json &loop, const Json &definitions) {
    std::vector<std::string> bodies;
    for (const auto &body : loopBranchBodies(loop))
        if (!body.empty() && definitions.contains(body) &&
            std::ranges::find(bodies, body) == bodies.end())
            bodies.push_back(body);
    return bodies;
}

// A name proposed from a port's: what the node id rule takes (no slash), and
// no `[]` -- the loop spells an output per element with it.
std::string loopPortBase(std::string_view base, std::string_view fallback) {
    std::string name{base};
    if (name.ends_with("[]")) name.resize(name.size() - 2);
    std::ranges::replace(name, '/', '_');
    std::ranges::replace(name, '.', '_');
    return name.empty() ? std::string{fallback} : name;
}

// Whether a set_outputs endpoint may name `port` of this node: a graph.input's
// `value`, an instance's or a loop's outputs (the expansion answers for
// those), or a port the node declares -- what the engine's check reads.
bool givesOutput(const Json &node, std::string_view port) {
    const auto op = text(node, "op");
    if (op == "graph.input") return port == "value";
    if (op == "graph.instance" || op == "graph.loop") return true;
    // GL-1f: listed in `outputs` once something reads it.
    if (!forwardedInputOf(node, port).empty()) return true;
    const auto outputs = node.find("outputs");
    return outputs != node.end() && outputs->is_array() &&
           std::ranges::any_of(*outputs, [&](const Json &output) {
               return output.is_string() && output.get<std::string>() == port;
           });
}

} // namespace

std::string GraphCanvasModel::suggestLoopInputName(std::string_view loop,
                                                   std::string_view base) const {
    const auto name = loopPortBase(base, "input");
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return name;
    const auto bodies = distinctLoopBodies(*node, definitions_);
    const auto inputs = node->value("inputs", Json::object());
    const auto taken = [&](const std::string &candidate) {
        if (inputs.is_object() && inputs.contains(candidate)) return true;
        return std::ranges::any_of(bodies, [&](const std::string &body) {
            return findNode(body, candidate) != nullptr;
        });
    };
    if (!taken(name)) return name;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = name + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return name;
}

std::string GraphCanvasModel::suggestLoopOutputName(std::string_view loop,
                                                    std::string_view base) const {
    const auto name = loopPortBase(base, "output");
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return name;
    const auto bodies = distinctLoopBodies(*node, definitions_);
    const auto declared = node->value("outputs", Json::array());
    const auto taken = [&](const std::string &candidate) {
        if (declared.is_array() &&
            std::ranges::any_of(declared, [&](const Json &output) {
                return output == candidate || output == candidate + "[]";
            }))
            return true;
        return std::ranges::any_of(bodies, [&](const std::string &body) {
            const auto outputs = definitions_.at(body).value("outputs", Json::object());
            return outputs.is_object() && outputs.contains(candidate);
        });
    };
    if (!taken(name)) return name;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = name + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return name;
}

Json GraphCanvasModel::zoneInputOps(std::string_view loop, const std::string &source_node,
                                    const std::string &source_port, const std::string &body,
                                    const std::string &target_node,
                                    const std::string &target_port) const {
    Json ops = Json::array();
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return ops;
    // A loop reading its own output would be a cycle; the source is a root
    // node, the zone's outside.
    if (source_node.empty() || source_port.empty() || source_node == loop ||
        findNode({}, source_node) == nullptr)
        return ops;
    const auto bodies = distinctLoopBodies(*node, definitions_);
    if (bodies.empty()) return ops;
    const bool targeted = !target_node.empty();
    if (targeted && (target_port.empty() || std::ranges::find(bodies, body) == bodies.end() ||
                     findNode(body, target_node) == nullptr))
        return ops;
    const auto name = suggestLoopInputName(loop, source_port);
    // Every branch's body gets the port: the engine compares their
    // boundaries (graph.loop_branch_interface@1) at the end of the list. No
    // default -- the loop's edge is what it reads, a resource as often as a
    // number.
    for (const auto &each : bodies)
        ops.push_back(Json{{"op", "add_node"}, {"scope", each},
                           {"node", Json{{"id", name}, {"op", "graph.input"}}}});
    ops.push_back(Json{{"op", "set_input"}, {"scope", "root"}, {"node", std::string{loop}},
                       {"port", name},
                       {"source", Json{{"node", source_node}, {"port", source_port}}}});
    if (targeted)
        ops.push_back(Json{{"op", "set_input"}, {"scope", body}, {"node", target_node},
                           {"port", target_port},
                           {"source", Json{{"node", name}, {"port", "value"}}}});
    return ops;
}

bool GraphCanvasModel::addZoneInput(const std::string &loop, const std::string &source_node,
                                    const std::string &source_port, const std::string &body,
                                    const std::string &target_node,
                                    const std::string &target_port) {
    if (!canEdit()) return false;
    auto ops = zoneInputOps(loop, source_node, source_port, body, target_node, target_port);
    if (ops.empty()) return false;
    return submitOps(std::move(ops));
}

Json GraphCanvasModel::zoneOutputOps(std::string_view loop, const std::string &body,
                                     const std::string &node, const std::string &port,
                                     std::string *refusal) const {
    Json ops = Json::array();
    const auto *loop_node = findNode({}, loop);
    if (loop_node == nullptr || !isLoop(*loop_node)) return ops;
    const auto bodies = distinctLoopBodies(*loop_node, definitions_);
    if (std::ranges::find(bodies, body) == bodies.end()) return ops;
    const auto *maker = findNode(body, node);
    if (maker == nullptr || port.empty() || !givesOutput(*maker, port)) return ops;
    const auto name = suggestLoopOutputName(loop, port);
    const auto branches = loopBranchBodies(*loop_node);
    for (const auto &each : bodies) {
        auto outputs = definitions_.at(each).value("outputs", Json::object());
        if (!outputs.is_object()) outputs = Json::object();
        if (each != body) {
            // Every body gives the loop the same outputs. Another body gives
            // this one from its node of the same id and port -- the same
            // wiring, which is what a copied body has -- or not at all.
            const auto *same = findNode(each, node);
            if (same == nullptr || !givesOutput(*same, port)) {
                if (refusal != nullptr) {
                    const auto branch = std::distance(
                        branches.begin(), std::ranges::find(branches, each));
                    *refusal = "Branch " + std::to_string(branch) + " of loop " +
                               std::string{loop} + " runs " + each + ", which has no " + node +
                               "." + port + " to give " + name +
                               " from: add the output there first.";
                }
                return Json::array();
            }
        }
        outputs[name] = Json{{"node", node}, {"port", port}};
        ops.push_back(Json{{"op", "set_outputs"}, {"scope", each}, {"outputs", std::move(outputs)}});
    }
    // The loop hands on the last element's value as `p`; `p[]` is the Loop
    // fields' to add.
    auto replaced = *loop_node;
    if (!replaced.contains("outputs") || !replaced.at("outputs").is_array())
        replaced["outputs"] = Json::array();
    replaced["outputs"].push_back(name);
    ops.push_back(Json{{"op", "replace_node"}, {"scope", "root"}, {"node", std::move(replaced)}});
    return ops;
}

bool GraphCanvasModel::addZoneOutput(const std::string &loop, const std::string &body,
                                     const std::string &node, const std::string &port) {
    if (!canEdit()) return false;
    std::string refusal;
    auto ops = zoneOutputOps(loop, body, node, port, &refusal);
    if (ops.empty()) {
        if (!refusal.empty()) edit_notice_ = {InspectorNoticeKind::Error, refusal};
        return false;
    }
    return submitOps(std::move(ops));
}

bool GraphCanvasModel::addLoopCarry(const std::string &loop, const std::string &from,
                                    const std::string &to) {
    const auto read = this->loop(loop);
    if (!read || from.empty() || to.empty()) return false;
    std::vector<std::pair<std::string, std::string>> carry;
    for (const auto &pair : read->carry) {
        if (pair.from == from && pair.to == to) return false;
        carry.emplace_back(pair.from, pair.to);
    }
    carry.emplace_back(from, to);
    return setLoopCarry(loop, carry);
}

bool GraphCanvasModel::removeLoopCarry(const std::string &loop, const std::string &from,
                                       const std::string &to) {
    const auto read = this->loop(loop);
    if (!read) return false;
    std::vector<std::pair<std::string, std::string>> carry;
    bool found = false;
    for (const auto &pair : read->carry) {
        if (pair.from == from && pair.to == to) {
            found = true;
            continue;
        }
        carry.emplace_back(pair.from, pair.to);
    }
    return found && setLoopCarry(loop, carry);
}

std::string GraphCanvasModel::loopOriginScope(std::string_view loop, std::int64_t element,
                                              std::string_view node) const {
    const auto *found = findNode({}, loop);
    if (found == nullptr || !isLoop(*found)) return {};
    const auto bodies = loopBranchBodies(*found);
    const auto branch = branchForElement(loop, element);
    if (!branch || *branch >= bodies.size() || bodies[*branch].empty()) return {};
    if (findNode(bodies[*branch], node) == nullptr) return {};
    return bodies[*branch];
}

std::string GraphCanvasModel::showLoopOrigin(std::string_view loop, std::int64_t element,
                                             std::string_view node) {
    leaveFolded();
    const auto *found = findNode({}, loop);
    if (found == nullptr || !isLoop(*found)) return {};
    const auto bodies = loopBranchBodies(*found);
    const auto branch = branchForElement(loop, element);
    if (!branch || *branch >= bodies.size() || bodies[*branch].empty()) return {};
    const auto body = bodies[*branch];
    if (findNode(body, node) == nullptr) return {};
    loop_branch_[std::string{loop}] = *branch;
    if (breadcrumb_.size() != 1 || !entered_loop_.empty()) {
        breadcrumb_ = {std::string{}};
        entered_loop_.clear();
        enteredScope();
    }
    return body;
}

namespace {

// RTL-2s, decision 3: the place of render.compute's parameters a ray
// tracing refusal names, as the fields name a parameter
// ("ray_tracing.hit_shaders.<ray type>.<group>[.<stage>]",
// "ray_tracing.misses.<miss>", "ray_tracing.traced_ray_types[.<ray type>]",
// "ray_tracing.raygen", ...); empty for a code that is not the ray tracing
// task parser's (rtl1b's table).
std::string rayTracingRefusalParameter(const std::string &code, const Json &fields) {
    const bool ray = code.starts_with("pelican.ray_tracing.") || code == "pelican.ray_group_name_unknown@1" ||
                     code == "pelican.procedural_group_requires_intersection@1" ||
                     code == "pelican.ray_group_type_declared_in_task@1" ||
                     code == "pelican.ray_profile_authored_anyhit@1";
    if (!ray || !fields.is_object()) return {};
    const auto ray_type = text(fields, "ray_type");
    const auto group = text(fields, "group");
    const auto miss = text(fields, "miss");
    const auto stage = text(fields, "stage");
    std::string place = "ray_tracing";
    if (code == "pelican.ray_group_name_unknown@1") {
        const auto kind = text(fields, "kind");
        const auto name = text(fields, "name");
        if (kind == "miss") return place + ".misses" + (name.empty() ? "" : "." + name);
        if (kind == "ray_type") return place + ".traced_ray_types" + (name.empty() ? "" : "." + name);
        if (kind == "group") return place + ".hit_shaders";
        return place;
    }
    if (code == "pelican.ray_tracing.traced_ray_types_invalid@1")
        return place + ".traced_ray_types" + (ray_type.empty() ? "" : "." + ray_type);
    if (code == "pelican.ray_tracing.invalid_recursion@1") return place + ".max_recursion";
    if (code == "pelican.ray_tracing.callable_records_invalid@1") return place + ".callable_records";
    if (!miss.empty()) return place + ".misses." + miss;
    if (code.starts_with("pelican.ray_tracing.miss_shader_")) return place + ".misses";
    if (!ray_type.empty()) {
        place += ".hit_shaders." + ray_type;
        if (group.empty()) return place;
        place += "." + group;
        if (code == "pelican.procedural_group_requires_intersection@1" || stage == "intersection")
            return place + ".intersection";
        if (code == "pelican.ray_profile_authored_anyhit@1") return place + ".anyhit";
        // RTL-3s, decision 6 (engine EB-3): the record's and the payload's
        // refusals name the stage of the cell.
        if (stage == "closesthit" || stage == "anyhit") return place + "." + stage;
        return place;
    }
    if (code == "pelican.ray_tracing.hit_shader_missing@1") return place + ".hit_shaders";
    if (stage == "raygen") return place + ".raygen";
    return place;
}

} // namespace

void GraphCanvasModel::markRayTracingRefusal(const Json &error, const std::string &sentence) {
    markRefusal(error, sentence, {});
    // RTL-2s-c: met elsewhere (the Ray layout section's edit), so the canvas
    // and the node's fields draw it on their next look.
    ++report_revision_;
}

void GraphCanvasModel::markRefusal(const Json &error, const std::string &reason,
                                   const std::string &scope) {
    refusal_marks_.clear();
    parameter_refusal_.reset();
    scalar_target_fix_.reset();
    if (!error.is_object()) return;
    // The engine puts a named refusal's fields beside its code; a relayed
    // one may carry them as error_fields.
    const auto fields_member = error.find("error_fields");
    const Json &fields = fields_member != error.end() && fields_member->is_object()
                             ? *fields_member
                             : error;
    const auto code = text(error, "code");
    const auto mark = [&](const std::string &scope, const std::string &node) {
        if (node.empty() || findNode(scope, node) == nullptr) return;
        GraphCanvasRefusalMark entry{scope, node, reason};
        if (std::ranges::find(refusal_marks_, entry) == refusal_marks_.end())
            refusal_marks_.push_back(std::move(entry));
    };
    // A node of one element's copy: the loop, and the body node it copies,
    // in the scope of the branch that element ran.
    if (const auto origin = fields.find("loop_origin");
        origin != fields.end() && origin->is_object()) {
        const auto loop_id = text(*origin, "loop");
        mark({}, loop_id);
        const auto *loop_node = findNode({}, loop_id);
        const auto element = origin->value("element", std::int64_t{0});
        if (loop_node != nullptr && isLoop(*loop_node)) {
            const auto bodies = loopBranchBodies(*loop_node);
            const auto branch = branchForElement(loop_id, element);
            if (branch && *branch < bodies.size() && !bodies[*branch].empty())
                mark(bodies[*branch], text(*origin, "node"));
        }
        return;
    }
    // UX-12a, decision 4: a node refused by its parameter's value (CT-3), or
    // for one it lacks (RS-9), in the scope the edit was made in.
    // The fields may be quoted in the message of a broader refusal.
    if (const auto invalid = graphRefusalFields(error, "graph.render_node_parameter_invalid@1")) {
        const auto node = text(*invalid, "node");
        mark(scope, node);
        if (!node.empty())
            parameter_refusal_ =
                GraphCanvasParameterRefusal{scope, node, text(*invalid, "parameter"), reason};
        return;
    }
    if (const auto missing = graphRefusalFields(error, "graph.render_node_parameters_missing@1")) {
        mark(scope, text(*missing, "node"));
        return;
    }
    // RTL-2s, decision 3 (engine RTL-1b / RTL-1c): the ray tracing task
    // parser's refusal names its task -- a graph node's also its `node` --
    // and the ray type, group, miss or stage: the node is marked and its ray
    // tracing red at that place.
    if (const auto place = rayTracingRefusalParameter(code, fields); !place.empty()) {
        auto node = text(fields, "node");
        if (node.empty()) node = text(fields, "task");
        const auto in = findNode(scope, node) != nullptr ? scope : std::string{};
        if (!node.empty() && findNode(in, node) != nullptr) {
            mark(in, node);
            parameter_refusal_ = GraphCanvasParameterRefusal{in, node, place, reason};
            return;
        }
    }
    // UX-13b, decision 3 (engine CT-5): a buffer a render.scalar cannot
    // write. Named with its sink, it is the graph's: one edit makes it what
    // the engine expects. Without one it was declared outside the graph.
    if (const auto incompatible = graphRefusalFields(error, "graph.render_scalar_target_incompatible@1")) {
        GraphCanvasScalarTargetFix fix;
        fix.target = text(*incompatible, "target");
        if (const auto expected = incompatible->find("expected");
            expected != incompatible->end() && expected->is_object())
            if (const auto size = expected->find("size");
                size != expected->end() && size->is_number_integer() && size->get<std::int64_t>() >= 0)
                fix.size = size->get<std::uint64_t>();
        const auto node = text(*incompatible, "node");
        if (!node.empty() && !fix.target.empty()) {
            mark(scope, node);
            if (findNode(scope, fix.target) != nullptr) {
                fix.scope = scope;
                fix.in_graph = true;
            } else if (findNode({}, fix.target) != nullptr) {
                fix.in_graph = true;
            }
        }
        if (!fix.target.empty()) scalar_target_fix_ = std::move(fix);
        return;
    }
    // A loop refused by name.
    if (code.starts_with("graph.loop_")) {
        const auto node = fields.find("node");
        if (node != fields.end() && node->is_string()) mark({}, node->get<std::string>());
    }
}

std::vector<GraphCanvasCatalogEntry> GraphCanvasModel::paletteOperations(
    std::string_view text_filter, std::string_view domain) const {
    return paletteOperationsIn(currentScope(), text_filter, domain);
}

std::vector<GraphCanvasCatalogEntry> GraphCanvasModel::paletteOperationsIn(
    std::string_view scope, std::string_view text_filter, std::string_view domain) const {
    const auto lowered = [](std::string_view value) {
        std::string result{value};
        std::ranges::transform(result, result.begin(), [](unsigned char letter) {
            return static_cast<char>(std::tolower(letter));
        });
        return result;
    };
    const auto needle = lowered(text_filter);
    const auto matches = [&](const GraphCanvasCatalogEntry &entry) {
        if (!domain.empty() && entry.domain != domain) return false;
        return needle.empty() || lowered(entry.op).find(needle) != std::string::npos;
    };
    std::vector<GraphCanvasCatalogEntry> result;
    for (const auto &[op, declaration] : operations_.items()) {
        // The two structural operations are added below, on the side of the
        // boundary each belongs to, and never taken from the catalog.
        if (op == "graph.input" || op == "graph.instance" || op == "graph.loop") continue;
        // An operation whose ports follow its parameters cannot be authored
        // from a name alone: the palette does not pretend otherwise. LP-4c
        // makes one exception, render.fullscreen, which arrives whole: the
        // engine's passthrough drawing into an image of its own (addNodeOps).
        // LP-4e: and two more families, each on its own side: a material
        // pass in the root, which arrives with its attachments, and the draw
        // steps in a body a draw_loop runs.
        // UX-3, decision 1: and the rest of the render nodes, each seeded
        // with what it needs -- a raster pass only with the shaders it
        // starts from, a compute task only with a shader of the project's,
        // none of them in a draw_loop's body.
        const bool body_of_draws = drawBodyScope(scope);
        const bool seeded =
            op == kFullscreenTemplate || (op == kMaterialPass && scope.empty()) ||
            (isDrawStep(op) && body_of_draws) ||
            (!body_of_draws &&
             ((isPaletteSeededRender(op) &&
               (op != "render.compute" || !compute_shader_choices_.empty() ||
                !project_ray_tracing_choices_.empty() ||
                !engine_shaders_known_ || !rayTracingPipelines().empty())) ||
              (op == "render.raster" && rasterTemplateAvailable())));
        if (text(declaration, "ports") != "declared" && !seeded) continue;
        GraphCanvasCatalogEntry entry{op, text(declaration, "domain"),
                                      text(declaration, "evaluation"),
                                      declaration.value("sink", false), false};
        if (matches(entry)) result.push_back(std::move(entry));
    }
    std::ranges::sort(result, {}, &GraphCanvasCatalogEntry::op);
    // A boundary port belongs to a definition body; an instance stands in the
    // root document. Offering either on the wrong side would only earn a named
    // refusal from the engine.
    if (scope.empty()) {
        if (!definitions_.empty()) {
            GraphCanvasCatalogEntry entry{"graph.instance", "structure", "direct", false, true};
            if (matches(entry)) result.push_back(std::move(entry));
        }
        // LP-4c: a loop stands in the root too, and can bring its body with
        // it (a new definition), so it is offered with no definition yet.
        GraphCanvasCatalogEntry loop_entry{"graph.loop", "structure", "direct", false, true};
        if (matches(loop_entry)) result.push_back(std::move(loop_entry));
    } else {
        GraphCanvasCatalogEntry entry{"graph.input", "structure", "direct", false, true};
        if (matches(entry)) result.push_back(std::move(entry));
    }
    return result;
}

std::vector<std::string> GraphCanvasModel::paletteDomains() const {
    return paletteDomainsIn(currentScope());
}

std::vector<std::string> GraphCanvasModel::paletteDomainsIn(std::string_view scope) const {
    std::set<std::string> domains;
    for (const auto &entry : paletteOperationsIn(scope)) domains.insert(entry.domain);
    return {domains.begin(), domains.end()};
}

InspectorNotice GraphCanvasModel::notice() const {
    // An edit the user just attempted outranks everything: it is the one thing
    // they are waiting to hear about.
    if (edit_notice_.kind == InspectorNoticeKind::Error) return edit_notice_;
    if (failure_.kind != InspectorNoticeKind::None) return failure_;
    if (edit_notice_.kind != InspectorNoticeKind::None) return edit_notice_;
    if (context_received_ && !catalog_received_)
        return {InspectorNoticeKind::Information,
                "Operation catalog not fetched: ports and domains come from the "
                "authored document alone."};
    if (context_received_ && !definitions_reported_)
        return {InspectorNoticeKind::Information,
                "The engine reported no definitions: only the root scope can be shown."};
    if (!values_instance_.empty() && values_received_ && !values_published_)
        return {InspectorNoticeKind::Information,
                values_instance_ + " is not published: no drawn generation has fed its inputs."};
    // The node values are a separate report and can be unpublished on their
    // own -- a definition scope has no instance inputs to be unpublished.
    if (!values_instance_.empty() && node_values_received_ && !node_values_published_)
        return {InspectorNoticeKind::Information,
                values_instance_ +
                    " is not published: no drawn generation has left node values."};
    return {};
}

// --- LP-4e: the draw_loop stage, render.material_pass and draw.*.

const std::vector<std::string> &GraphCanvasModel::materialPassContracts() {
    // renderpipeline.cpp's materialPassContractFromName, less the legacy one:
    // a draw_loop does not run over it (graph.loop_contract_unsupported@1).
    static const std::vector<std::string> contracts{
        "forward_opaque_v1", "deferred_geometry_v1", "forward_transparent_v1"};
    return contracts;
}

const std::vector<std::string> &GraphCanvasModel::materialShaderContracts() {
    // What a draw_loop's `when.contract` compares (graphcomposition.cpp).
    static const std::vector<std::string> contracts{
        "gbuffer_v1", "forward_scene_color_v1", "legacy_gbuffer_v1"};
    return contracts;
}

std::vector<std::string> GraphCanvasModel::materialPassColorSockets(const Json &parameters) {
    std::vector<std::string> sockets;
    if (!parameters.is_object()) return sockets;
    if (const auto outputs = parameters.find("material_outputs"); outputs != parameters.end()) {
        // {schema: pelican.material_outputs, outputs: [{name, ...}]}
        if (outputs->is_object())
            for (const auto &output : outputs->value("outputs", Json::array()))
                if (output.is_object() && !text(output, "name").empty())
                    sockets.push_back(text(output, "name"));
        return sockets;
    }
    const auto contract = text(parameters, "material_contract");
    if (contract == "forward_opaque_v1" || contract == "forward_transparent_v1") return {"color"};
    if (contract == "deferred_geometry_v1" || contract == "legacy_gbuffer_v1")
        return {"albedo", "normal", "material", "worldpos", "emissive"};
    return sockets;
}

bool GraphCanvasModel::drawBodyScope(std::string_view scope) const {
    if (scope.empty()) return false;
    const auto found = definitions_.find(std::string{scope});
    if (found == definitions_.end() || !found->is_object()) return false;
    for (const auto &loop : nodes_) {
        if (!isDrawItemsLoop(loop)) continue;
        const auto bodies = loopBranchBodies(loop);
        if (std::ranges::find(bodies, scope) != bodies.end()) return true;
    }
    for (const auto &node : found->value("nodes", Json::array()))
        if (node.is_object() && isDrawStep(text(node, "op"))) return true;
    return false;
}

bool GraphCanvasModel::setLoopWhereTags(const std::string &id, const GraphCanvasTagFilter &tags) {
    auto parameters = loopParameters(id);
    if (tags.empty()) parameters.erase("where");
    else parameters["where"] = Json{{"tags", tagFilterJson(tags)}};
    return setLoopParameters(id, std::move(parameters));
}

bool GraphCanvasModel::setLoopBranchDrawWhen(const std::string &id, std::size_t branch,
                                             const GraphCanvasDrawWhen &when) {
    // An empty condition is the else, which only the last row is; a tag
    // test with no tag in it tests nothing.
    if (when.empty() || (when.tags && when.tags->empty())) return false;
    if (when.material && when.material->empty()) return false;
    if (when.contract && std::ranges::find(materialShaderContracts(), *when.contract) ==
                             materialShaderContracts().end())
        return false;
    auto parameters = loopParameters(id);
    auto branches = parameters.value("branches", Json::array());
    if (!branches.is_array() || branch + 1 >= branches.size()) return false;
    auto &row = branches[branch];
    if (!row.is_object()) return false;
    row["when"] = drawWhenJson(when);
    parameters["branches"] = std::move(branches);
    return setLoopParameters(id, std::move(parameters));
}

std::vector<std::string> GraphCanvasModel::loopMaterials(std::string_view id) const {
    std::vector<std::string> materials;
    const auto *report = loopReport(id);
    if (report == nullptr) return materials;
    for (const auto &element : report->value("elements", Json::array()))
        if (element.is_object() && element.contains("element") && element.at("element").is_string()) {
            auto key = element.at("element").get<std::string>();
            if (std::ranges::find(materials, key) == materials.end())
                materials.push_back(std::move(key));
        }
    return materials;
}

bool GraphCanvasModel::setDrawVariant(const std::string &node, const std::string &variant) {
    return setDrawVariant(currentScope(), node, variant);
}

bool GraphCanvasModel::setDrawVariant(std::string_view scope, const std::string &node,
                                      const std::string &variant) {
    const auto *found = findNode(scope, node);
    if (found == nullptr || text(*found, "op") != "draw.variant" || variant.empty()) return false;
    if (text(nodeParameters(scope, node), "variant") == variant) return false;
    return setNodeParameter(scope, node, "variant", variant);
}

bool GraphCanvasModel::setDrawOverride(const std::string &node, const Json &render_state,
                                       const std::vector<std::string> &defines) {
    return setDrawOverride(currentScope(), node, render_state, defines);
}

bool GraphCanvasModel::setDrawOverride(std::string_view scope, const std::string &node,
                                       const Json &render_state,
                                       const std::vector<std::string> &defines) {
    if (!canEdit()) return false;
    const auto *found = findNode(scope, node);
    if (found == nullptr || text(*found, "op") != "draw.override") return false;
    if (!render_state.is_object()) return false;
    // An override that replaces nothing is refused by the engine: the step
    // would draw exactly what draw.material draws.
    if (render_state.empty() && defines.empty()) return false;
    if (std::ranges::any_of(defines, [](const std::string &define) { return define.empty(); }))
        return false;
    auto parameters = nodeParameters(scope, node);
    if (render_state.empty()) parameters.erase("render_state");
    else parameters["render_state"] = render_state;
    if (defines.empty()) parameters.erase("defines");
    else parameters["defines"] = defines;
    if (parameters == nodeParameters(scope, node)) return false;
    return submitOps(Json::array({Json{
        {"op", "set_node_parameters"}, {"scope", scopeToken(scope)}, {"node", node},
        {"parameters", std::move(parameters)}}}));
}

bool GraphCanvasModel::setMaterialPassContract(const std::string &node, const std::string &contract) {
    const auto *found = findNode(currentScope(), node);
    if (found == nullptr || text(*found, "op") != kMaterialPass) return false;
    if (std::ranges::find(materialPassContracts(), contract) == materialPassContracts().end())
        return false;
    if (text(nodeParameters(node), "material_contract") == contract) return false;
    return setNodeParameter(node, "material_contract", contract);
}

std::string GraphCanvasModel::suggestDrawVariant() const {
    return suggestDrawVariant(currentScope());
}

std::string GraphCanvasModel::suggestDrawVariant(std::string_view scope) const {
    for (const auto &loop : nodes_) {
        if (!isDrawItemsLoop(loop)) continue;
        const auto bodies = loopBranchBodies(loop);
        if (std::ranges::find(bodies, scope) == bodies.end()) continue;
        const auto id = text(loop, "id");
        for (const auto &pass : nodes_) {
            if (!pass.is_object() || text(pass, "op") != kMaterialPass) continue;
            const auto draws = pass.value("inputs", Json::object()).value("draws", Json{});
            if (!draws.is_object() || text(draws, "node") != id) continue;
            const auto variant = text(pass.value("parameters", Json::object()), "material_variant");
            if (!variant.empty()) return variant;
        }
    }
    return "variant";
}

// --- Q-19: a render node's input sockets, and lines into them.

std::string GraphCanvasInputSocket::key() const {
    if (kind == Kind::variadic_drop) return "in:variadic";
    if (kind == Kind::undeclared) return port;
    return "in@" + std::to_string(binding);
}

void GraphCanvasModel::readPlanPassInputs(const Json &plan) {
    plan_pass_inputs_.clear();
    plan_loop_copies_.clear();
    const auto nodes = plan.find("nodes");
    if (nodes == plan.end() || !nodes->is_array()) return;
    for (std::size_t index = 0; index < nodes->size(); ++index) {
        const auto &node = nodes->at(index);
        if (!node.is_object()) continue;
        const auto name = text(node, "name");
        if (name.empty()) continue;
        try {
            auto inputs = framePlanPassInputs(
                node, "get_frame_plan.nodes[" + std::to_string(index) + "]");
            if (!inputs) continue;
            plan_pass_inputs_.insert_or_assign(name, std::move(*inputs));
        } catch (const std::exception &) {
            // A declaration Studio cannot read leaves the node its rows.
            continue;
        }
        const auto origin = node.find("loop_origin");
        if (origin == node.end() || !origin->is_object()) continue;
        const auto element = origin->find("element");
        if (element == origin->end() || !element->is_number_integer()) continue;
        FramePlanLoopOrigin read{text(*origin, "loop"), element->get<std::int64_t>(),
                                 text(*origin, "node")};
        if (read.loop.empty() || read.node.empty()) continue;
        plan_loop_copies_.push_back(PlanLoopCopy{std::move(read), name});
    }
}

const FramePlanPassInputs *GraphCanvasModel::passInputs(std::string_view scope,
                                                        std::string_view node) const {
    // UX-5b, decision 1: an engine shader the catalog declared stands for
    // the plan's declarations while the plan still runs another fragment
    // (the pick has not been compiled yet), or does not list the pass.
    const auto catalog = [&](const FramePlanPassInputs *planned) -> const FramePlanPassInputs * {
        if (planned != nullptr && planned->declared() && planned->fragment.empty()) return planned;
        const auto shader = nodeParameters(scope, node).value("shader", Json::object());
        const auto fragment = shader.is_object() ? text(shader, "fragment") : std::string{};
        const auto declared = engine_shader_inputs_.find(fragment);
        if (declared == engine_shader_inputs_.end()) return planned;
        if (planned != nullptr && planned->declared() && planned->fragment == fragment)
            return planned;
        return &declared->second;
    };
    const auto find = [&](std::string_view name) -> const FramePlanPassInputs * {
        const auto found = plan_pass_inputs_.find(name);
        return found == plan_pass_inputs_.end() ? nullptr : &found->second;
    };
    if (!engine_shader_inputs_.empty()) {
        const auto *planned = passInputsFromPlan(scope, node, find);
        return catalog(planned);
    }
    return passInputsFromPlan(scope, node, find);
}

const FramePlanPassInputs *GraphCanvasModel::passInputsFromPlan(
    std::string_view scope, std::string_view node,
    const std::function<const FramePlanPassInputs *(std::string_view)> &find) const {
    // A root node's pass is named by its id (a copy's name holds slashes,
    // which an id cannot).
    if (scope.empty()) return find(node);
    // A body node: the first copy, in the plan's order, of an element whose
    // branch runs this body. Every copy draws with the same shader.
    for (const auto &copy : plan_loop_copies_) {
        if (copy.origin.node != node) continue;
        const auto *loop = findNode({}, copy.origin.loop);
        if (loop == nullptr || !isLoop(*loop)) continue;
        const auto bodies = loopBranchBodies(*loop);
        const auto branch = branchForElement(copy.origin.loop, copy.origin.element);
        const bool runs = branch ? *branch < bodies.size() && bodies[*branch] == scope
                                 : std::ranges::find(bodies, scope) != bodies.end();
        if (!runs) continue;
        if (const auto *found = find(copy.name)) return found;
    }
    // Or an instance of it, whose passes are `<instance>/<id>`.
    for (const auto &candidate : nodes_) {
        if (!candidate.is_object() || text(candidate, "op") != "graph.instance" ||
            text(candidate, "definition") != scope)
            continue;
        if (const auto *found = find(text(candidate, "id") + "/" + std::string{node}))
            return found;
    }
    return nullptr;
}

bool GraphCanvasModel::planMissesRenderNode() const {
    const auto misses = [&](std::string_view scope) {
        const auto *nodes = scopeNodes(scope);
        if (nodes == nullptr) return false;
        return std::ranges::any_of(*nodes, [&](const Json &node) {
            if (!node.is_object() || !isGraphicsPass(text(node, "op"))) return false;
            const auto *inputs = passInputs(scope, text(node, "id"));
            return inputs == nullptr || !inputs->declared();
        });
    };
    if (misses({})) return true;
    for (const auto &node : nodes_) {
        if (!node.is_object()) continue;
        if (text(node, "op") == "graph.instance") {
            if (misses(text(node, "definition"))) return true;
            continue;
        }
        if (!isLoop(node)) continue;
        // A loop the report says ran nothing has no copy to read.
        if (const auto *report = loopReport(text(node, "id"));
            report != nullptr && report->contains("count") && report->at("count").is_number() &&
            report->at("count").get<double>() <= 0.0)
            continue;
        for (const auto &body : distinctLoopBodies(node, definitions_))
            if (misses(body)) return true;
    }
    return false;
}

std::map<std::int64_t, std::string> GraphCanvasModel::numberedPorts(const Json &parameters) {
    std::map<std::int64_t, std::string> numbered;
    if (!parameters.is_object()) return numbered;
    const auto ports = parameters.find("ports");
    if (ports == parameters.end() || !ports->is_object()) return numbered;
    for (const auto &[port, declaration] : ports->items()) {
        if (!declaration.is_object() || text(declaration, "binding") != "input") continue;
        const auto index = declaration.find("input_index");
        if (index == declaration.end() || !index->is_number_integer() ||
            index->get<std::int64_t>() < 0)
            continue;
        numbered.emplace(index->get<std::int64_t>(), port);
    }
    return numbered;
}

std::string GraphCanvasModel::suggestInputPortName(std::string_view scope, std::string_view node,
                                                   std::string_view base) const {
    const auto name = loopPortBase(base, "input");
    const auto *found = findNode(scope, node);
    if (found == nullptr) return name;
    const auto parameters = found->value("parameters", Json::object());
    const auto ports = parameters.is_object() ? parameters.value("ports", Json::object())
                                              : Json::object();
    const auto inputs = found->value("inputs", Json::object());
    const auto taken = [&](const std::string &candidate) {
        return (ports.is_object() && ports.contains(candidate)) ||
               (inputs.is_object() && inputs.contains(candidate));
    };
    if (!taken(name)) return name;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = name + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return name;
}

Json GraphCanvasModel::imageTypeOf(std::string_view scope, std::string_view node,
                                   std::string_view port, int depth) const {
    const auto *source = depth > 0 ? findNode(scope, node) : nullptr;
    if (source == nullptr) return sceneColorType();
    const auto op = text(*source, "op");
    const auto typed = [](const Json &type) {
        return type.is_object() && type.contains("semantic") && type.at("semantic").is_string();
    };
    // A body's boundary port carries what the loop or the instance running
    // the body is fed there.
    if (op == "graph.input" && !scope.empty()) {
        for (const auto &runner : nodes_) {
            if (!runner.is_object()) continue;
            const bool runs =
                (text(runner, "op") == "graph.instance" && text(runner, "definition") == scope) ||
                (isLoop(runner) && runsBody(runner, scope));
            if (!runs) continue;
            const auto inputs = runner.value("inputs", Json::object());
            const auto edge = inputs.is_object() ? inputs.value(std::string{node}, Json{}) : Json{};
            if (edge.is_object())
                return imageTypeOf({}, text(edge, "node"), text(edge, "port"), depth - 1);
        }
        return sceneColorType();
    }
    // An instance's or a loop's output is what its body's output is.
    if (op == "graph.instance" || isLoop(*source)) {
        const auto body = op == "graph.instance" ? text(*source, "definition")
                                                 : loopEntryBody(*source);
        std::string output{port};
        if (output.ends_with("[]")) output.resize(output.size() - 2);
        const auto definition = definitions_.find(body);
        if (definition == definitions_.end()) return sceneColorType();
        const auto outputs = definition->value("outputs", Json::object());
        const auto endpoint = outputs.is_object() ? outputs.value(output, Json{}) : Json{};
        if (!endpoint.is_object()) return sceneColorType();
        return imageTypeOf(body, text(endpoint, "node"), text(endpoint, "port"), depth - 1);
    }
    // GL-1f: a pass that hands on what it wrote is what its input is fed.
    if (!forwardedInputOf(*source, port).empty()) {
        const auto type = forwardedImageType(scope, node, forwardedInputOf(*source, port), depth);
        return typed(type) ? type : sceneColorType();
    }
    const auto parameters = source->value("parameters", Json::object());
    if (parameters.is_object()) {
        // A pass's port says what it writes there; an image, a target
        // reference or a history read says what it is.
        const auto ports = parameters.value("ports", Json::object());
        if (ports.is_object())
            if (const auto declared = ports.find(std::string{port});
                declared != ports.end() && declared->is_object() && declared->contains("type") &&
                typed(declared->at("type")))
                return declared->at("type");
        if (const auto type = parameters.find("type"); type != parameters.end() && typed(*type))
            return *type;
    }
    return sceneColorType();
}

Json GraphCanvasModel::socketOps(std::string_view scope, const std::string &node,
                                 const std::string &socket, const std::string &source_node,
                                 const std::string &source_port, const std::string &base,
                                 const Json &type, std::string *refusal) const {
    const auto *target = findNode(scope, node);
    if (target == nullptr || !isGraphicsPass(text(*target, "op"))) return Json::array();
    if (source_node.empty() || source_port.empty() || source_node == node) return Json::array();
    const auto *declared = passInputs(scope, node);
    if (declared == nullptr || !declaresPassInputs(*declared)) return Json::array();
    const auto refuse = [&](std::string line) {
        if (refusal != nullptr) *refusal = std::move(line);
        return Json::array();
    };
    auto parameters = nodeParameters(scope, node);
    const auto numbered = numberedPorts(parameters);
    // The inputs fill the bindings in order: the next one is the first
    // number no port holds.
    std::size_t count = 0;
    while (numbered.contains(static_cast<std::int64_t>(count))) ++count;
    const auto fill_first = [&](std::size_t from, std::size_t to_exclusive) {
        return refuse("Connect binding " + bindingRange(from, to_exclusive) + " of " + node +
                      " first: a pass's inputs fill the bindings in order.");
    };
    std::size_t binding = 0;
    std::string existing;
    if (socket == "in:variadic") {
        if (!declared->variadic_range) return refuse(node + "'s shader has no variadic group.");
        const auto [first, max] = *declared->variadic_range;
        if (count < first) return fill_first(count, first);
        if (count - first >= max)
            return refuse(node + "'s shader takes at most " + std::to_string(max) +
                          " inputs in its variadic group.");
        binding = count;
    } else if (socket.starts_with("in@")) {
        const auto digits = std::string_view{socket}.substr(3);
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), binding);
        if (digits.empty() || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
            return Json::array();
        const auto declaration = std::ranges::find_if(declared->declarations, [&](const auto &entry) {
            return !entry.variadic && entry.binding == binding;
        });
        const bool member =
            declared->variadic_range && binding >= declared->variadic_range->first &&
            binding < declared->variadic_range->first + declared->variadic_range->max &&
            std::ranges::any_of(declared->declarations, &FramePlanPassInputDeclaration::variadic);
        if (declaration == declared->declarations.end() && !member)
            return refuse(node + "'s shader declares no binding " + std::to_string(binding) + ".");
        if (declaration != declared->declarations.end() && declaration->buffer()) {
            // UX-8, decision 2: a buffer binding reads a named buffer port --
            // the one it reads already, else one named for the declaration
            // -- added with its line in one list. The engine hands a pass's
            // buffer reads over as storage buffers only.
            if (declaration->type.find("storage") == std::string::npos)
                return refuse("Binding " + std::to_string(binding) + " of " + node + " is a " +
                              declaration->type +
                              ": a pass hands its shader storage buffers (declare it as a "
                              "`readonly buffer` block).");
            // The engine binds a named port only to the variable the port
            // header would declare for it: pelican_resource_<port>.
            if (!declaration->name.starts_with(kResourcePortVariablePrefix))
                return refuse(
                    "Binding " + std::to_string(binding) + " of " + node + " is " +
                    (declaration->name.empty() ? std::string{"an unnamed block"}
                                               : declaration->name) +
                    ": the engine reads a buffer port through a variable named "
                    "pelican_resource_<port>. Name the block's variable so, or read it "
                    "through pelican_resource_ports.glsl and + Buffer port.");
            auto port = bufferSocketPort(*declared, bufferPortReads(scope, node), *declaration);
            if (port.empty()) {
                port = bufferPortName(declaration->name);
                const auto ports = parameters.value("ports", Json::object());
                if (ports.is_object() && ports.contains(port) &&
                    text(ports.at(port), "kind") != "buffer")
                    port = suggestInputPortName(scope, node, port);
            }
            return bufferPortOps(scope, node, port, source_node, source_port, refusal);
        }
        if (binding > count) return fill_first(count, binding);
        if (binding < count) existing = numbered.at(static_cast<std::int64_t>(binding));
    } else {
        // An extra's socket is named by its port: a line dropped there
        // feeds that port instead.
        const auto found = std::ranges::find_if(
            numbered, [&](const auto &entry) { return entry.second == socket; });
        if (found == numbered.end()) return Json::array();
        existing = socket;
        binding = static_cast<std::size_t>(found->first);
    }
    const auto scope_token = scopeToken(scope);
    const Json source{{"node", source_node}, {"port", source_port}};
    Json ops = Json::array();
    if (!existing.empty()) {
        // In place of what is connected there: the port stays at its number
        // and its line moves; its type follows the new source.
        const auto inputs = target->value("inputs", Json::object());
        if (inputs.is_object() && inputs.value(existing, Json{}) == source) return Json::array();
        auto &declaration = parameters["ports"][existing];
        if (declaration.value("type", Json{}) != type) {
            declaration["type"] = type;
            ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", scope_token},
                               {"node", node}, {"parameters", parameters}});
        }
        ops.push_back(Json{{"op", "set_input"}, {"scope", scope_token}, {"node", node},
                           {"port", existing}, {"source", source}});
        return ops;
    }
    const auto name = suggestInputPortName(scope, node, base);
    parameters["ports"][name] = Json{{"kind", "image"},        {"type", type},
                                     {"access", "read"},       {"binding", "input"},
                                     {"input_index", binding}};
    clampFullscreenInput(text(*target, "op"), parameters["ports"][name]);
    ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", scope_token}, {"node", node},
                       {"parameters", std::move(parameters)}});
    ops.push_back(Json{{"op", "set_input"}, {"scope", scope_token}, {"node", node},
                       {"port", name}, {"source", source}});
    return ops;
}

Json GraphCanvasModel::socketConnectOps(std::string_view scope, const std::string &node,
                                        const std::string &socket,
                                        const std::string &source_node,
                                        const std::string &source_port,
                                        std::string *refusal) const {
    const auto *source = findNode(scope, source_node);
    if (source == nullptr) return Json::array();
    // A boundary port's one output is `value`; its id says what it is.
    const auto base = text(*source, "op") == "graph.input" ? source_node : source_port;
    return socketOps(scope, node, socket, source_node, source_port, base,
                     imageTypeOf(scope, source_node, source_port, 8), refusal);
}

bool GraphCanvasModel::connectSocket(std::string_view scope, const std::string &node,
                                     const std::string &socket, const std::string &source_node,
                                     const std::string &source_port) {
    if (!canEdit()) return false;
    std::string refusal;
    auto ops = socketConnectOps(scope, node, socket, source_node, source_port, &refusal);
    if (ops.empty()) {
        if (!refusal.empty()) edit_notice_ = {InspectorNoticeKind::Error, std::move(refusal)};
        return false;
    }
    return submitOps(std::move(ops));
}

Json GraphCanvasModel::removeInputPortOps(std::string_view scope, const std::string &node,
                                          const std::string &port) const {
    const auto *target = findNode(scope, node);
    if (target == nullptr || !isGraphicsPass(text(*target, "op"))) return Json::array();
    auto parameters = nodeParameters(scope, node);
    const auto numbered = numberedPorts(parameters);
    const auto found =
        std::ranges::find_if(numbered, [&](const auto &entry) { return entry.second == port; });
    if (found == numbered.end()) return Json::array();
    const auto removed = found->first;
    parameters["ports"].erase(port);
    for (const auto &[index, other] : numbered)
        if (index > removed) parameters["ports"][other]["input_index"] = index - 1;
    const auto scope_token = scopeToken(scope);
    return Json::array({
        Json{{"op", "set_input"}, {"scope", scope_token}, {"node", node}, {"port", port},
             {"source", nullptr}},
        Json{{"op", "set_node_parameters"}, {"scope", scope_token}, {"node", node},
             {"parameters", std::move(parameters)}}});
}

bool GraphCanvasModel::addZoneInputToSocket(const std::string &loop, const std::string &source_node,
                                            const std::string &source_port, const std::string &body,
                                            const std::string &target_node,
                                            const std::string &socket) {
    if (!canEdit()) return false;
    const auto *loop_node = findNode({}, loop);
    if (loop_node == nullptr || !isLoop(*loop_node)) return false;
    const auto bodies = distinctLoopBodies(*loop_node, definitions_);
    if (std::ranges::find(bodies, body) == bodies.end()) return false;
    auto ops = zoneInputOps(loop, source_node, source_port);
    if (ops.empty()) return false;
    // The graph.input the list adds, which the port reads: named as
    // zoneInputOps names it, and carrying what the root source carries.
    const auto name = suggestLoopInputName(loop, source_port);
    std::string refusal;
    auto socket_ops = socketOps(body, target_node, socket, name, "value", name,
                                imageTypeOf({}, source_node, source_port, 8), &refusal);
    if (socket_ops.empty()) {
        if (!refusal.empty()) edit_notice_ = {InspectorNoticeKind::Error, std::move(refusal)};
        return false;
    }
    for (auto &op : socket_ops) ops.push_back(std::move(op));
    return submitOps(std::move(ops));
}

// --- GL-1: features and the rendering section as folded nodes.

namespace {

std::vector<std::string> planStrings(const Json &node, const char *key) {
    std::vector<std::string> result;
    const auto found = node.find(key);
    if (found == node.end() || !found->is_array()) return result;
    for (const auto &value : *found)
        if (value.is_string()) result.push_back(value.get<std::string>());
    return result;
}

// "bloom" for "engine://features/bloom.json" -- frameplanedit's
// graphTemplateFeatureName, which this library does not link.
std::string featureShortName(const std::string &feature) {
    const auto slash = feature.rfind('/');
    auto name = slash == std::string::npos ? feature : feature.substr(slash + 1);
    if (name.ends_with(".json")) name.resize(name.size() - 5);
    return name.empty() ? feature : name;
}

void appendUnique(std::vector<std::string> &list, const std::string &value) {
    if (std::ranges::find(list, value) == list.end()) list.push_back(value);
}

bool listed(const std::vector<std::string> &list, const std::string &value) {
    return std::ranges::find(list, value) != list.end();
}

// What a folded node reads from outside itself (a read no earlier pass of
// it wrote) and hands on (a write something outside it reads, or nothing
// reads at all).
void foldedBoundary(GraphCanvasFoldedNode &node, const std::vector<GraphCanvasPlanPass> &plan) {
    const std::set<std::string, std::less<>> inside(node.passes.begin(), node.passes.end());
    std::set<std::string, std::less<>> written;
    for (const auto &pass : plan) {
        if (!inside.contains(pass.name)) continue;
        for (const auto *reads : {&pass.reads, &pass.history_reads})
            for (const auto &resource : *reads)
                if (!written.contains(resource)) appendUnique(node.reads, resource);
        written.insert(pass.writes.begin(), pass.writes.end());
    }
    for (const auto &pass : plan) {
        if (!inside.contains(pass.name)) continue;
        for (const auto &resource : pass.writes) {
            bool read = false;
            bool outside = false;
            for (const auto &other : plan)
                if (listed(other.reads, resource) || listed(other.history_reads, resource)) {
                    read = true;
                    if (!inside.contains(other.name)) outside = true;
                }
            if (outside || !read) appendUnique(node.writes, resource);
        }
    }
}

GraphCanvasFoldedNode singlePassNode(const GraphCanvasPlanPass &pass) {
    GraphCanvasFoldedNode node;
    node.id = pass.name;
    node.label = pass.name;
    node.type = pass.material ? "material" : pass.kind;
    node.source = pass.source;
    node.feature = pass.feature;
    node.material = pass.material;
    node.passes.push_back(pass.name);
    return node;
}

} // namespace

std::string GraphCanvasFoldedNode::tooltip() const {
    if (rendering_section) return "From the rendering section: edit it here.";
    // §4 rule 3: readable here; expanded to be edited.
    const auto owner = !feature.empty() ? "feature " + featureShortName(feature)
                       : !preset.empty() ? "preset " + preset
                                         : "the preset (" + source + ")";
    return "From " + owner + ": read only here. Expand to edit\xE2\x80\xA6";
}

std::string GraphCanvasDomainStub::text() const {
    return std::string{input ? "\xE2\x86\x90 " : "\xE2\x86\x92 "} + domain + " (" +
           std::to_string(count) + ")";
}

void GraphCanvasModel::readPlanPasses(const Json &plan) {
    plan_graph_ = text(plan, "graph");
    // The contract each material pass draws with, as material_routing says.
    std::map<std::string, std::string, std::less<>> contracts;
    // UX-11, decision 4: and the routes that name each.
    std::map<std::string, std::vector<std::string>, std::less<>> pass_routes;
    if (const auto routing = plan.find("material_routing");
        routing != plan.end() && routing->is_object())
        if (const auto routes = routing->find("routes");
            routes != routing->end() && routes->is_object())
            for (const auto &[route, value] : routes->items()) {
                if (!value.is_object()) continue;
                const auto pass = text(value, "pass");
                if (!pass.empty() && !contracts.contains(pass))
                    contracts[pass] = text(value, "contract");
                if (!pass.empty()) pass_routes[pass].push_back(route);
            }
    std::vector<GraphCanvasPlanPass> passes;
    const auto nodes = plan.find("nodes");
    if (nodes != plan.end() && nodes->is_array())
        for (std::size_t index = 0; index < nodes->size(); ++index) {
            const auto &node = nodes->at(index);
            if (!node.is_object() || text(node, "name").empty()) continue;
            GraphCanvasPlanPass pass;
            pass.name = text(node, "name");
            pass.authored_name = text(node, "authored_name");
            pass.kind = text(node, "kind");
            pass.source = text(node, "source");
            pass.feature = text(node, "provider_feature");
            pass.provider_reference = text(node, "provider_ref");
            pass.reads = planStrings(node, "reads");
            pass.history_reads = planStrings(node, "reads_history");
            pass.writes = planStrings(node, "writes");
            pass.order = index;
            if (const auto order = node.find("order");
                order != node.end() && order->is_number_integer() && order->get<std::int64_t>() >= 0)
                pass.order = static_cast<std::size_t>(order->get<std::int64_t>());
            if (const auto origin = node.find("loop_origin");
                origin != node.end() && origin->is_object()) {
                pass.loop = text(*origin, "loop");
                pass.loop_node = text(*origin, "node");
                if (const auto element = origin->find("element");
                    element != origin->end() && element->is_number_integer())
                    pass.loop_element = element->get<std::int64_t>();
            }
            if (const auto references = node.find("loop_outputs");
                references != node.end() && references->is_array())
                for (const auto &entry : *references)
                    if (entry.is_object() && !text(entry, "reference").empty())
                        pass.loop_outputs.push_back(
                            {text(entry, "reference"), text(entry, "resource")});
            // GL-2b: the graph node that made it, and the feature passes
            // it took the place of.
            if (const auto origin = node.find("origin");
                origin != node.end() && origin->is_object() && !text(*origin, "node").empty()) {
                GraphCanvasPlanOrigin from;
                from.graph = text(*origin, "graph");
                from.node = text(*origin, "node");
                from.definition = text(*origin, "definition");
                from.instance_path = text(*origin, "instance_path");
                from.loop = text(*origin, "loop");
                if (const auto branch = origin->find("loop_branch");
                    branch != origin->end() && branch->is_number_unsigned())
                    from.loop_branch = branch->get<std::size_t>();
                if (const auto element = origin->find("element");
                    element != origin->end() && element->is_number_integer())
                    from.element = element->get<std::int64_t>();
                pass.origin = std::move(from);
            }
            if (const auto replaces = node.find("replaces");
                replaces != node.end() && replaces->is_array())
                for (const auto &entry : *replaces)
                    if (entry.is_object() && !text(entry, "pass").empty())
                        pass.replaces.push_back({text(entry, "feature"), text(entry, "pass")});
            pass.material_variant = text(node, "material_variant");
            const auto resolution = node.find("shader_resolution");
            const bool material_owned = resolution != node.end() && resolution->is_object() &&
                                        text(*resolution, "state") == "material_owned";
            const auto filter = node.find("material_filter");
            const bool filtered = filter != node.end() && filter->is_object();
            pass.material = material_owned || filtered || !pass.material_variant.empty();
            if (filtered)
                if (const auto count = filter->find("resolved_draw_count");
                    count != filter->end() && count->is_number_unsigned())
                    pass.draw_count = count->get<std::size_t>();
            if (filtered) {
                pass.material_include = planStrings(*filter, "include");
                pass.material_exclude = planStrings(*filter, "exclude");
            }
            if (const auto routes = pass_routes.find(pass.name); routes != pass_routes.end())
                pass.routes = routes->second;
            if (const auto contract = contracts.find(pass.name); contract != contracts.end())
                pass.contract = contract->second;
            // LC-1: present (and true) only for a shader that reads it.
            if (const auto reads = node.find("reads_loop_index");
                reads != node.end() && reads->is_boolean())
                pass.reads_loop_index = reads->get<bool>();
            // PS-1s (engine PS-1): a once node, and when it last ran (null
            // before it has).
            pass.once = text(node, "schedule") == "once";
            for (auto [key, field] :
                 {std::pair{"last_run_generation", &pass.last_run_generation},
                  std::pair{"last_run_frame", &pass.last_run_frame}})
                if (const auto value = node.find(key);
                    value != node.end() && value->is_number_unsigned())
                    *field = value->get<std::uint64_t>();
            // UX-12c (engine MP-1): what it drew; a list Studio cannot read
            // is the Frame Plan tab's to name, and here it is not there.
            try {
                pass.drawn_materials = framePlanDrawnMaterials(node, "get_frame_plan node");
            } catch (const std::exception &) {
            }
            passes.push_back(std::move(pass));
        }
    std::ranges::stable_sort(passes, {}, &GraphCanvasPlanPass::order);
    plan_passes_ = std::move(passes);
    // PS-1s, decision 4: a Recompute is followed until a pass it scheduled
    // says it ran after the press.
    if (once_follow_reads_ > 0 &&
        std::ranges::any_of(once_follow_from_, [&](const auto &entry) {
            const auto *pass = planPass(entry.first);
            return pass != nullptr && pass->last_run_frame &&
                   *pass->last_run_frame > entry.second;
        })) {
        once_follow_reads_ = 0;
        once_follow_from_.clear();
    }
    // UX-12c, decision 1: followed until a pass says what it drew.
    if (std::ranges::any_of(plan_passes_, [](const GraphCanvasPlanPass &pass) {
            return pass.drawn_materials.has_value();
        }))
        drawn_follow_reads_ = 0;
    // GL-1e: a pass's `input` counts images and buffers apart.
    plan_buffers_.clear();
    if (const auto resources = plan.find("resources");
        resources != plan.end() && resources->is_array())
        for (const auto &resource : *resources)
            if (resource.is_object() && text(resource, "kind") == "buffer")
                plan_buffers_.insert(text(resource, "name"));
    readPlanLayers(plan);
    // Decision 3: which features have a decomposed version, asked once.
    if (connected_ && !templates_requested_ &&
        std::ranges::any_of(plan_passes_, [](const GraphCanvasPlanPass &pass) {
            return !pass.feature.empty();
        })) {
        templates_requested_ = true;
        read(RequestKind::Templates, "get_graph_templates", Json::object());
    }
}

const GraphCanvasPlanPass *GraphCanvasModel::planPass(std::string_view name) const {
    const auto found = std::ranges::find(plan_passes_, name, &GraphCanvasPlanPass::name);
    return found == plan_passes_.end() ? nullptr : &*found;
}

std::string GraphCanvasModel::graphOwnerOf(const GraphCanvasPlanPass &pass) const {
    // GL-2b (design_graph_layers 5.1): the engine says which graph node made
    // a pass. The root node it is drawn under is the root instance its
    // instance path starts at, else the loop whose copy it is, else the node
    // itself. A pass with no origin is the preset's, a feature's or the
    // rendering section's -- whatever its name or its `source` says.
    if (plan_origin_aware_) {
        if (!pass.origin) return {};
        const auto &origin = *pass.origin;
        if (!origin.instance_path.empty())
            return origin.instance_path.substr(0, origin.instance_path.find('/'));
        if (!origin.loop.empty()) return origin.loop;
        return origin.node;
    }
    // An engine that predates origin (GL-1): read off the document -- the
    // loop its loop_origin names, the root instance whose expansion named it
    // `<instance>/...`, a root node whose id is its name, or a render node
    // whose id is its authored name. Its `source` cannot say it: under an
    // engine:// preset a graph node's pass reads "engine" as the preset's own
    // do. A pass none of these match reads as the rendering section's.
    if (!pass.loop.empty()) return pass.loop;
    const auto &root = realNodes();
    for (const auto &node : root) {
        if (!node.is_object() || text(node, "op") != "graph.instance") continue;
        const auto id = text(node, "id");
        if (!id.empty() && pass.name.starts_with(id + "/")) return id;
    }
    for (const auto &node : root)
        if (node.is_object() && !text(node, "id").empty() && text(node, "id") == pass.name)
            return pass.name;
    if (!pass.authored_name.empty())
        for (const auto &node : root)
            if (node.is_object() && text(node, "id") == pass.authored_name &&
                takesViewSelect(text(node, "op")))
                return pass.authored_name;
    return {};
}

std::string GraphCanvasModel::foldedOwnerOf(const GraphCanvasPlanPass &pass) const {
    // An anchor places passes; it is not one.
    if (pass.kind == "anchor" || !graphOwnerOf(pass).empty()) return {};
    return pass.feature.empty() ? pass.name : "feature:" + featureShortName(pass.feature);
}

std::vector<GraphCanvasFoldedNode> GraphCanvasModel::rootFoldedNodes() const {
    std::vector<GraphCanvasFoldedNode> result;
    for (const auto &pass : plan_passes_) {
        const auto id = foldedOwnerOf(pass);
        if (id.empty()) continue;
        auto found = std::ranges::find(result, id, &GraphCanvasFoldedNode::id);
        if (found == result.end()) {
            if (pass.feature.empty()) {
                result.push_back(singlePassNode(pass));
                continue;
            }
            GraphCanvasFoldedNode node;
            node.id = id;
            node.label = featureShortName(pass.feature);
            node.type = "feature";
            node.source = pass.source;
            node.feature = pass.feature;
            node.feature_group = true;
            node.decomposable = templates_.contains(pass.feature);
            result.push_back(std::move(node));
            found = std::prev(result.end());
        }
        found->passes.push_back(pass.name);
    }
    for (auto &node : result) {
        foldedBoundary(node, plan_passes_);
        markFoldedOwner(node);
    }
    return result;
}

bool GraphCanvasModel::renderingSectionPass(std::string_view pass) const {
    const auto graphs = render_authoring_.find("graphs");
    if (graphs == render_authoring_.end() || !graphs->is_array()) return false;
    for (const auto &graph : *graphs) {
        if (!graph.is_object() || text(graph, "name") != plan_graph_) continue;
        const auto passes = graph.find("passes");
        if (passes == graph.end() || !passes->is_array()) return false;
        for (const auto &entry : *passes)
            if (entry.is_object() && text(entry, "name") == pass)
                return entry.value("editable_root_pass", false);
    }
    return false;
}

void GraphCanvasModel::markFoldedOwner(GraphCanvasFoldedNode &node) const {
    node.rendering_section = !node.feature_group && node.feature.empty() &&
                             node.passes.size() == 1 && renderingSectionPass(node.passes.front());
    if (node.rendering_section || !node.feature.empty()) return;
    if (const auto preset = render_authoring_.find("pipeline_preset");
        preset != render_authoring_.end() && preset->is_object())
        node.preset = text(*preset, "name");
}

std::vector<GraphCanvasFoldedNode> GraphCanvasModel::featurePassNodes(std::string_view feature) const {
    std::vector<GraphCanvasFoldedNode> result;
    for (const auto &pass : plan_passes_) {
        if (pass.feature != feature || pass.kind == "anchor") continue;
        result.push_back(singlePassNode(pass));
        foldedBoundary(result.back(), plan_passes_);
        markFoldedOwner(result.back());
    }
    return result;
}

std::vector<GraphCanvasFoldedNode> GraphCanvasModel::foldedNodes() const {
    if (!currentScope().empty()) return {};
    if (folded_stash_) {
        if (folded_trail_.empty() || folded_trail_.back().kind != "feature") return {};
        return featurePassNodes(folded_trail_.back().feature);
    }
    if (!domainShown("render")) return {};
    return rootFoldedNodes();
}

std::vector<GraphCanvasDerivedEdge> GraphCanvasModel::derivedEdges() const {
    std::vector<GraphCanvasDerivedEdge> result;
    if (!currentScope().empty()) return result;
    const bool root = !folded_stash_;
    std::string feature;
    if (!root) {
        if (folded_trail_.empty() || folded_trail_.back().kind != "feature") return result;
        feature = folded_trail_.back().feature;
    }
    std::set<std::string, std::less<>> derived;
    for (const auto &node : foldedNodes()) derived.insert(node.id);
    if (derived.empty()) return result;
    // The root's graph nodes on screen, and the ones a resource is named by.
    std::set<std::string, std::less<>> shown;
    std::map<std::string, std::string, std::less<>> resource_nodes;
    if (root)
        for (const auto &node : nodes({})) {
            if (!nodeShown({}, node)) continue;
            shown.insert(node.id);
            if (node.op == "render.target_ref") {
                const auto name = node.parameters.find("name");
                if (name != node.parameters.end() && name->is_string())
                    resource_nodes.emplace(name->get<std::string>(), node.id);
            } else if (node.op == "render.image" || node.op == "render.buffer" ||
                       node.op == "render.attribute") {
                resource_nodes.emplace(node.id, node.id);
            }
        }
    const auto owner = [&](const GraphCanvasPlanPass &pass) -> std::string {
        if (pass.kind == "anchor") return {};
        if (!root) return pass.feature == feature ? pass.name : std::string{};
        if (auto graph = graphOwnerOf(pass); !graph.empty())
            return shown.contains(graph) ? graph : std::string{};
        const auto id = foldedOwnerOf(pass);
        return derived.contains(id) ? id : std::string{};
    };
    const auto add = [&](const std::string &from, const std::string &to,
                         const std::string &resource) {
        const auto found = std::ranges::find_if(result, [&](const GraphCanvasDerivedEdge &edge) {
            return edge.from == from && edge.to == to;
        });
        if (found == result.end()) result.push_back({from, to, {resource}});
        else appendUnique(found->resources, resource);
    };
    for (const auto &reader : plan_passes_) {
        const auto reader_owner = owner(reader);
        if (reader_owner.empty()) continue;
        const auto read = [&](const std::string &resource, bool history) {
            // The pass that wrote what this one reads: the last before it,
            // or -- for last frame's -- the last of the frame.
            const GraphCanvasPlanPass *writer = nullptr;
            for (const auto &candidate : plan_passes_) {
                if (!history && candidate.order >= reader.order) break;
                if (listed(candidate.writes, resource)) writer = &candidate;
            }
            auto from = writer != nullptr ? owner(*writer) : std::string{};
            const auto resource_node = resource_nodes.find(resource);
            if (from.empty() && resource_node != resource_nodes.end()) from = resource_node->second;
            if (from.empty()) return;
            // A graph node reads the resource through the node that names it.
            auto to = reader_owner;
            if (!derived.contains(to) && resource_node != resource_nodes.end() &&
                resource_node->second != from)
                to = resource_node->second;
            if (from == to || (!derived.contains(from) && !derived.contains(to))) return;
            add(from, to, resource);
        };
        for (const auto &resource : reader.reads) read(resource, false);
        for (const auto &resource : reader.history_reads) read(resource, true);
    }
    return result;
}

std::string GraphCanvasModel::canvasNodeOfPass(std::string_view name) const {
    const auto *pass = planPass(name);
    if (pass == nullptr || pass->kind == "anchor") return {};
    if (auto graph = graphOwnerOf(*pass); !graph.empty()) return graph;
    return foldedOwnerOf(*pass);
}

bool GraphCanvasModel::graphWritesResourceNamedIn(std::string_view message) const {
    std::set<std::string, std::less<>> words;
    std::size_t at = 0;
    while (at < message.size()) {
        const auto start = message.find_first_not_of(" \t\r\n", at);
        if (start == std::string_view::npos) break;
        auto end = message.find_first_of(" \t\r\n", start);
        if (end == std::string_view::npos) end = message.size();
        auto word = message.substr(start, end - start);
        while (!word.empty() && (word.back() == ':' || word.back() == ',' ||
                                 word.back() == ';' || word.back() == '.'))
            word.remove_suffix(1);
        if (!word.empty()) words.emplace(word);
        at = end;
    }
    return std::ranges::any_of(plan_passes_, [&](const GraphCanvasPlanPass &pass) {
        return pass.kind != "anchor" && !graphOwnerOf(pass).empty() &&
               std::ranges::any_of(pass.writes, [&](const std::string &written) {
                   return words.contains(written);
               });
    });
}

// --- GL-1e: a rendering-section pass's inputs.

void GraphCanvasModel::requestRenderAuthoring() {
    // Only a plan with a pass of the rendering section has one to edit: a
    // project pass no graph node made and no feature brought.
    if (!connected_ ||
        std::ranges::none_of(plan_passes_, [this](const GraphCanvasPlanPass &pass) {
            return pass.source == "project" && pass.feature.empty() &&
                   pass.kind != "anchor" && graphOwnerOf(pass).empty();
        }))
        return;
    read(RequestKind::Authoring, "get_render_authoring_context", Json::object());
}

void GraphCanvasModel::takeRenderAuthoring(Json context) {
    render_authoring_ = std::move(context);
    ++authoring_revision_;
    // RTL-2s-c: the ray layout's names are get_ray_layout's (the Ray layout
    // section's reading); the context only says whether there is one.
    const auto found = render_authoring_.find("ray_layout");
    if (found == render_authoring_.end() || !found->is_object()) {
        ray_layout_read_.reset();
        return;
    }
    const auto reference = found->find("reference");
    if (reference != found->end() && reference->is_string()) {
        read(RequestKind::RayLayout, "get_ray_layout", Json::object());
        return;
    }
    // No layout: nothing to ask.
    RayLayoutRead none;
    if (const auto mode = found->find("mode"); mode != found->end() && mode->is_string())
        none.mode = mode->get<std::string>();
    ray_layout_read_ = std::move(none);
}

FramePlanModel GraphCanvasModel::passInputPlan() const {
    FramePlanModel plan;
    plan.graph = plan_graph_;
    for (const auto &pass : plan_passes_) {
        FramePlanNode node;
        node.name = pass.name;
        node.kind = pass.kind;
        node.source = pass.source;
        node.reads = pass.reads;
        node.history_reads = pass.history_reads;
        node.writes = pass.writes;
        node.loop_outputs = pass.loop_outputs;
        if (const auto inputs = plan_pass_inputs_.find(pass.name);
            inputs != plan_pass_inputs_.end())
            node.pass_inputs = inputs->second;
        plan.nodes.push_back(std::move(node));
    }
    for (const auto &buffer : plan_buffers_) {
        FramePlanResource resource;
        resource.name = buffer;
        resource.kind = "buffer";
        plan.resources.push_back(std::move(resource));
    }
    return plan;
}

std::string GraphCanvasModel::outputResource(std::string_view node, std::string_view port) const {
    return scopeOutputResource({}, node, port);
}

std::string GraphCanvasModel::scopeOutputResource(std::string_view scope, std::string_view node,
                                                  std::string_view port) const {
    std::string at{node};
    std::string socket{port};
    // A render node hands on what it wrote: the resource on its input of
    // the output's name (render.fullscreen's `color`), followed a few steps.
    for (int step = 0; step < 8; ++step) {
        const auto *found = findNode(scope, at);
        if (found == nullptr || !found->is_object()) return {};
        const auto op = text(*found, "op");
        if (op == "render.image" || op == "render.buffer" || op == "render.attribute")
            return socket == "resource" ? at : std::string{};
        if (op == "render.target_ref") {
            const auto parameters = found->find("parameters");
            return parameters != found->end() && parameters->is_object()
                       ? text(*parameters, "name")
                       : std::string{};
        }
        // UX-5a: a loop's output is no input of the same name (a loop's
        // `color` in and `color` out are two things). UX-5c: it is named as
        // the loop's (`loop.color`), and the engine resolves it.
        if (op == "graph.loop") return scope.empty() ? loopOutputResource(at, socket) : std::string{};
        const auto inputs = found->find("inputs");
        if (inputs == found->end() || !inputs->is_object()) return {};
        const auto input = inputs->find(socket);
        if (input == inputs->end() || !input->is_object()) return {};
        const auto source = text(*input, "node");
        if (source.empty()) return {};
        socket = text(*input, "port");
        at = source;
    }
    return {};
}

std::string GraphCanvasModel::loopOutputResource(const std::string &loop_id,
                                                 std::string_view port) const {
    // UX-5c: a pass of the rendering section names a loop's `p` as
    // `<loop>.<output>` (RS-13); the engine reads it as a graph reader of `p`
    // does -- the last element's value, whatever Count is. `p[]` is one value
    // per element, which no pass's input names (dropOnPassInput says so).
    if (port.empty() || port.ends_with("[]")) return {};
    const auto *found = findNode({}, loop_id);
    if (found == nullptr || text(*found, "op") != "graph.loop") return {};
    const auto outputs = found->find("outputs");
    if (outputs != found->end() && outputs->is_array() &&
        std::ranges::none_of(*outputs, [&](const Json &output) {
            return output.is_string() && output.get<std::string>() == port;
        }))
        return {};
    return loop_id + "." + std::string{port};
}

void GraphCanvasModel::setEditNotice(InspectorNotice notice) {
    edit_notice_ = std::move(notice);
    ++report_revision_;
}

// --- GL-2b: the plan's lower layers.

namespace {

bool meets(const std::vector<std::string> &names, const std::vector<std::string> &nodes) {
    return std::ranges::any_of(names, [&](const std::string &name) { return listed(nodes, name); });
}

std::string joinedNames(const std::vector<std::string> &names) {
    std::string result;
    for (const auto &name : names) result += (result.empty() ? "" : ", ") + name;
    return result;
}

// U+00B7 between the parts of one band or row; U+2192 between two ends.
constexpr std::string_view kDot = " \xC2\xB7 ";
constexpr std::string_view kArrow = " \xE2\x86\x92 ";

} // namespace

void GraphCanvasModel::readPlanLayers(const Json &plan) {
    plan_barriers_.clear();
    plan_resources_.clear();
    plan_alias_groups_.clear();
    plan_scopes_.clear();
    plan_decisions_.clear();
    plan_candidates_.clear();
    plan_access_.clear();
    plan_diagnostics_.clear();
    deactivated_nodes_.clear();
    plan_fold_decisions_.clear();
    // GL-5f, decision 3: which resources are a loop's copies.
    plan_resource_origins_.clear();
    if (const auto resources = plan.find("resources"); resources != plan.end() && resources->is_array())
        for (const auto &resource : *resources) {
            if (!resource.is_object()) continue;
            const auto origin = resource.find("loop_origin");
            if (origin == resource.end() || !origin->is_object()) continue;
            plan_resource_origins_[text(resource, "name")] =
                FramePlanLoopOrigin{text(*origin, "loop"), origin->value("element", std::int64_t{0}),
                                    text(*origin, "node")};
        }
    // An engine with GL-2a always publishes these two, empty or not.
    plan_origin_aware_ = plan.contains("diagnostics") || plan.contains("deactivated_graph_nodes") ||
                         std::ranges::any_of(plan_passes_, [](const GraphCanvasPlanPass &pass) {
                             return pass.origin.has_value();
                         });
    const auto array = [](const Json &object, const char *key) -> const Json * {
        const auto found = object.find(key);
        return found != object.end() && found->is_array() ? &*found : nullptr;
    };
    if (const auto *barriers = array(plan, "barriers"))
        for (const auto &barrier : *barriers)
            if (barrier.is_object())
                plan_barriers_.push_back({text(barrier, "from"), text(barrier, "to"),
                                          text(barrier, "resource"), text(barrier, "kind")});
    const auto readDecisions = [&](const Json &container) {
        if (const auto *decisions = array(container, "decisions"))
            for (const auto &decision : *decisions)
                if (decision.is_object())
                    plan_decisions_.push_back({text(decision, "id"), text(decision, "subject"),
                                               text(decision, "selected"),
                                               planStrings(decision, "from_nodes")});
    };
    if (const auto physical = plan.find("physical_target_plan");
        physical != plan.end() && physical->is_object()) {
        if (const auto *resources = array(*physical, "resources"))
            for (const auto &resource : *resources)
                if (resource.is_object())
                    plan_resources_.push_back({text(resource, "logical_resource"),
                                               text(resource, "format"),
                                               planStrings(resource, "from_nodes")});
        if (const auto *groups = array(*physical, "alias_groups"))
            for (const auto &group : *groups)
                if (group.is_object())
                    plan_alias_groups_.push_back({text(group, "id"), planStrings(group, "resources"),
                                                  planStrings(group, "from_nodes")});
        if (const auto *scopes = array(*physical, "scopes"))
            for (const auto &scope : *scopes)
                if (scope.is_object())
                    plan_scopes_.push_back(
                        {text(scope, "id"), text(scope, "kind"), planStrings(scope, "nodes")});
        readDecisions(*physical);
        for (const char *key : {"lowering_graph", "planning_opportunities", "backend_selection"})
            if (const auto found = physical->find(key); found != physical->end() && found->is_object())
                readDecisions(*found);
        if (const auto selection = physical->find("backend_selection");
            selection != physical->end() && selection->is_object()) {
            const auto selected = text(*selection, "selected_candidate");
            if (const auto *candidates = array(*selection, "candidates"))
                for (const auto &candidate : *candidates) {
                    if (!candidate.is_object()) continue;
                    PlanCandidate entry;
                    entry.candidate = text(candidate, "candidate");
                    entry.endpoint = text(candidate, "endpoint");
                    const auto feasible = candidate.find("feasible");
                    entry.feasible = feasible != candidate.end() && feasible->is_boolean() &&
                                     feasible->get<bool>();
                    entry.selected = !selected.empty() && entry.candidate == selected;
                    plan_candidates_.push_back(std::move(entry));
                }
        }
    }
    if (const auto execution = plan.find("execution_plan");
        execution != plan.end() && execution->is_object())
        if (const auto *nodes = array(*execution, "nodes"))
            for (const auto &node : *nodes) {
                if (!node.is_object()) continue;
                const auto name = text(node, "name");
                if (const auto pass = std::ranges::find(plan_passes_, name, &GraphCanvasPlanPass::name);
                    pass != plan_passes_.end())
                    pass->backend = text(node, "selected_endpoint");
                if (const auto *uses = array(node, "resource_uses"))
                    for (const auto &use : *uses)
                        if (use.is_object() && !text(use, "resource").empty())
                            plan_access_[name][text(use, "resource")] = text(use, "access");
            }
    if (const auto *diagnostics = array(plan, "diagnostics"))
        for (const auto &diagnostic : *diagnostics)
            if (diagnostic.is_object()) {
                plan_diagnostics_.push_back({text(diagnostic, "kind"), text(diagnostic, "subject"),
                                             text(diagnostic, "detail")});
                // UX-6b: PO-1's details are objects.
                if (const auto detail = diagnostic.find("detail");
                    detail != diagnostic.end() && detail->is_object()) {
                    plan_diagnostics_.back().resource = text(*detail, "resource");
                    if (const auto binding = detail->find("binding");
                        binding != detail->end() && binding->is_number_unsigned())
                        plan_diagnostics_.back().binding = binding->get<std::size_t>();
                }
            }
    if (const auto *deactivated = array(plan, "deactivated_graph_nodes"))
        for (const auto &node : *deactivated)
            if (node.is_object() && !text(node, "node").empty())
                deactivated_nodes_.push_back(
                    {text(node, "node"), text(node, "feature"), text(node, "reason")});
    // GL-3a (FG-1): the decisions verified fold groups added.
    if (const auto *folds = array(plan, "fold_decisions"))
        for (const auto &fold : *folds)
            if (fold.is_object() && !text(fold, "name").empty())
                plan_fold_decisions_.push_back({text(fold, "group"), text(fold, "kind"),
                                                text(fold, "name"), text(fold, "field")});
}

std::vector<std::string> GraphCanvasModel::foldDecisionParts(std::string_view canvas_node) const {
    std::vector<std::string> parts;
    if (plan_fold_decisions_.empty()) return parts;
    const auto passes = planNodesOf(canvas_node);
    // The resources the node hands on, for a resource's decision.
    std::vector<std::string> handed;
    for (const auto &node : nodes({}))
        if (node.id == canvas_node)
            for (const auto &output : node.outputs)
                if (auto resource = outputResource(node.id, output.port); !resource.empty())
                    handed.push_back(std::move(resource));
    for (const auto &decision : plan_fold_decisions_) {
        const bool here = decision.kind == "resource" ? listed(handed, decision.name)
                                                      : listed(passes, decision.name);
        if (!here) continue;
        auto part = decision.field + " (fold " + decision.group + ")";
        if (!listed(parts, part)) parts.push_back(std::move(part));
    }
    return parts;
}

std::string GraphCanvasModel::graphBackend() const {
    // The endpoint most passes run on is the graph's; a pass is marked only
    // when it runs elsewhere. With no pass saying, the selected candidate's.
    std::map<std::string, std::size_t, std::less<>> counts;
    for (const auto &pass : plan_passes_)
        if (!pass.backend.empty()) ++counts[pass.backend];
    std::string best;
    std::size_t most = 0;
    for (const auto &[backend, count] : counts)
        if (count > most) {
            best = backend;
            most = count;
        }
    if (!best.empty()) return best;
    for (const auto &candidate : plan_candidates_)
        if (candidate.selected) return candidate.endpoint;
    return {};
}

GraphCanvasPlanTarget GraphCanvasModel::planNodeTarget(std::string_view name) const {
    GraphCanvasPlanTarget target;
    const auto *pass = planPass(name);
    if (pass == nullptr || pass->kind == "anchor") return target;
    if (plan_origin_aware_ && pass->origin) {
        const auto &origin = *pass->origin;
        if (!origin.instance_path.empty()) {
            // Inside an instance: through the instances on its path, into
            // the body the node is in.
            target.kind = "instance";
            target.node = origin.node;
            target.scope = origin.definition;
            target.instance_path = origin.instance_path;
            return target;
        }
        if (!origin.loop.empty()) {
            // One copy of a loop: the body node, in the zone, on the tab of
            // the branch that element ran.
            target.kind = "loop";
            target.node = origin.node;
            target.scope = origin.definition;
            target.loop = origin.loop;
            target.element = origin.element.value_or(pass->loop_element);
            return target;
        }
        target.kind = "root";
        target.node = origin.node;
        return target;
    }
    // An engine that predates origin: a loop's copy by its loop_origin.
    if (!plan_origin_aware_ && !pass->loop.empty() && !pass->loop_node.empty()) {
        target.kind = "loop";
        target.node = pass->loop_node;
        target.loop = pass->loop;
        target.element = pass->loop_element;
        return target;
    }
    const auto id = canvasNodeOfPass(name);
    if (id.empty()) return target;
    target.kind = "root";
    target.node = id;
    target.folded = graphOwnerOf(*pass).empty();
    return target;
}

bool GraphCanvasModel::planPassEditedHere(std::string_view name) const {
    const auto target = planNodeTarget(name);
    if (target.kind.empty()) return false;
    // A node in a body: an instance's, or a loop's copy.
    if (target.kind != "root") return true;
    // A graph node of the root, or a folded node standing for the pass.
    if (findNode({}, target.node) != nullptr) return true;
    return renderingSectionPass(name);
}

std::vector<std::string> GraphCanvasModel::planNodesOf(std::string_view canvas_node) const {
    std::vector<std::string> result;
    if (canvas_node.empty()) return result;
    for (const auto &pass : plan_passes_) {
        if (pass.kind == "anchor") continue;
        auto owner = graphOwnerOf(pass);
        if (owner.empty()) owner = foldedOwnerOf(pass);
        if (owner == canvas_node) result.push_back(pass.name);
    }
    return result;
}

// GL-5f, decision 3.
std::vector<GraphCanvasLayerRow> GraphCanvasModel::bodyLayerRows(std::string_view scope,
                                                                 std::string_view node) const {
    std::vector<GraphCanvasLayerRow> rows;
    if (scope.empty() || node.empty() || folded_stash_ || plan_passes_.empty()) return rows;
    const std::string dot{kDot};
    for (const auto &[name, origin] : plan_resource_origins_) {
        if (origin.node != node || loopOriginScope(origin.loop, origin.element, origin.node) != scope)
            continue;
        auto line = "resource " + name + dot + "loop " + origin.loop + " [" +
                    std::to_string(origin.element) + "]";
        const auto found = std::ranges::find(plan_resources_, name, &PlanResource::name);
        if (found != plan_resources_.end() && !found->format.empty()) line += dot + found->format;
        rows.push_back({"L2", "resource", line, {}, name});
    }
    return rows;
}

std::optional<FramePlanLoopOrigin> GraphCanvasModel::planResourceLoopOrigin(
    std::string_view resource) const {
    const auto found = plan_resource_origins_.find(resource);
    if (found == plan_resource_origins_.end()) return std::nullopt;
    return found->second;
}

bool GraphCanvasModel::layersShown() const {
    return currentScope().empty() && !folded_stash_ && domains_shown_.size() == 1 &&
           domains_shown_.contains("render") && !plan_passes_.empty();
}

const GraphCanvasDeactivatedNode *GraphCanvasModel::deactivatedNode(std::string_view node) const {
    const auto found = std::ranges::find(deactivated_nodes_, node, &GraphCanvasDeactivatedNode::node);
    return found == deactivated_nodes_.end() ? nullptr : &*found;
}

std::string GraphCanvasModel::layerBand(std::string_view canvas_node) const {
    if (!layersShown()) return {};
    const auto nodes = planNodesOf(canvas_node);
    std::vector<std::string> parts;
    // L1: how many copies -- a loop's elements, an instance's instances.
    std::set<std::string, std::less<>> copies;
    for (const auto &name : nodes) {
        const auto *pass = planPass(name);
        if (pass->origin)
            copies.insert(pass->origin->instance_path + "|" + pass->origin->loop + "|" +
                          (pass->origin->element ? std::to_string(*pass->origin->element) : ""));
        else
            copies.insert(pass->loop + "|" +
                          (pass->loop.empty() ? std::string{} : std::to_string(pass->loop_element)));
    }
    // A loop's zone already says how many copies on its bottom row.
    if (copies.size() > 1 && !loop(std::string{canvas_node}))
        parts.push_back("\xC3\x97" + std::to_string(copies.size()) + " copies");
    // L2: the barriers its passes take part in, and aliasing.
    const auto barriers = static_cast<std::size_t>(
        std::ranges::count_if(plan_barriers_, [&](const PlanBarrier &barrier) {
            return listed(nodes, barrier.from) || listed(nodes, barrier.to);
        }));
    if (barriers > 0)
        parts.push_back(std::to_string(barriers) + (barriers == 1 ? " barrier" : " barriers"));
    if (std::ranges::any_of(plan_alias_groups_, [&](const PlanAliasGroup &group) {
            return meets(group.from_nodes, nodes);
        }))
        parts.push_back("aliased");
    // L3: a backend other than the graph's.
    const auto graph = graphBackend();
    for (const auto &name : nodes) {
        const auto *pass = planPass(name);
        if (!pass->backend.empty() && pass->backend != graph && !listed(parts, pass->backend))
            parts.push_back(pass->backend);
    }
    // GL-3a, decision 6: what a verified fold group decided for it.
    for (auto &part : foldDecisionParts(canvas_node)) parts.push_back(std::move(part));
    if (const auto *off = deactivatedNode(canvas_node))
        parts.push_back("deactivated: " + off->reason +
                        (off->feature.empty() ? std::string{} : " (" + off->feature + ")"));
    std::string band;
    for (const auto &part : parts) band += (band.empty() ? std::string{} : std::string{kDot}) + part;
    return band;
}

bool GraphCanvasModel::nodeNoteDiagnostic(const GraphCanvasPlanDiagnostic &diagnostic) {
    return diagnostic.kind == "unread_output" || diagnostic.kind == "unused_binding";
}

std::vector<GraphCanvasPlanNote> GraphCanvasModel::planNotes(std::string_view canvas_node) const {
    std::vector<GraphCanvasPlanNote> notes;
    if (!layersShown()) return notes;
    const auto nodes = planNodesOf(canvas_node);
    for (const auto &diagnostic : plan_diagnostics_) {
        if (!nodeNoteDiagnostic(diagnostic) || !listed(nodes, diagnostic.subject)) continue;
        // A loop's copies name their own resources ("loop/2/..."); the note
        // is about the node, said once.
        auto resource = diagnostic.resource;
        if (const auto *pass = planPass(diagnostic.subject); pass != nullptr && !pass->loop.empty()) {
            const auto prefix = pass->loop + "/" + std::to_string(pass->loop_element) + "/";
            if (resource.starts_with(prefix)) resource = resource.substr(prefix.size());
        }
        GraphCanvasPlanNote note;
        if (diagnostic.kind == "unread_output") {
            note.band = "output unread";
            note.sentence = resource +
                            " is written but nothing reads it this frame \xE2\x80\x94 the pass may "
                            "be removable";
        } else {
            const auto binding = diagnostic.binding ? std::to_string(*diagnostic.binding)
                                                    : std::string{"?"};
            note.band = "binding " + binding + " unused";
            note.sentence = "binding " + binding + " (" + resource +
                            ") is declared but the shader never reads it";
        }
        if (std::ranges::find(notes, note) == notes.end()) notes.push_back(std::move(note));
    }
    return notes;
}

std::vector<GraphCanvasLayerRow> GraphCanvasModel::layerRows(std::string_view canvas_node) const {
    std::vector<GraphCanvasLayerRow> rows;
    if (!layersShown()) return rows;
    const auto nodes = planNodesOf(canvas_node);
    const std::string dot{kDot};
    const std::string arrow{kArrow};
    // L1: what it expanded to.
    for (const auto &name : nodes) {
        const auto *pass = planPass(name);
        auto line = name + dot + (pass->material ? std::string{"material"} : pass->kind);
        const auto &loop = pass->origin ? pass->origin->loop : pass->loop;
        if (!loop.empty()) {
            const auto element = pass->origin && pass->origin->element ? *pass->origin->element
                                                                        : pass->loop_element;
            line += dot + "loop " + loop + " [" + std::to_string(element) + "]";
        }
        if (pass->origin && !pass->origin->instance_path.empty())
            line += dot + "instance " + pass->origin->instance_path;
        rows.push_back({"L1", "pass", line, name});
        for (const auto &replaced : pass->replaces)
            rows.push_back({"L1", "replaces",
                            name + " replaces pass " + replaced.pass + " of " + replaced.feature, name});
    }
    // A feature's folded node: its passes a graph node took the place of.
    const auto folded = rootFoldedNodes();
    if (const auto feature = std::ranges::find(folded, canvas_node, &GraphCanvasFoldedNode::id);
        feature != folded.end() && feature->feature_group) {
        const auto short_name = featureShortName(feature->feature);
        std::vector<std::string> said;
        for (const auto &pass : plan_passes_)
            for (const auto &replaced : pass.replaces) {
                if (replaced.feature != short_name && replaced.feature != feature->feature) continue;
                auto owner = graphOwnerOf(pass);
                if (owner.empty()) owner = pass.name;
                const auto line = "pass " + replaced.pass + " is replaced by graph node " + owner;
                if (listed(said, line)) continue;
                said.push_back(line);
                rows.push_back({"L1", "replaced", line, pass.name});
            }
    }
    if (const auto *off = deactivatedNode(canvas_node))
        rows.push_back({"L1", "deactivated",
                        "deactivated: " + off->reason +
                            (off->feature.empty() ? std::string{} : " (feature " + off->feature + ")"),
                        {}});
    // L2: resources, alias groups, scopes and barriers.
    for (const auto &resource : plan_resources_) {
        if (!meets(resource.from_nodes, nodes)) continue;
        auto line = "resource " + resource.name;
        if (!resource.format.empty()) line += dot + resource.format;
        for (const auto &group : plan_alias_groups_)
            if (listed(group.resources, resource.name)) line += dot + "alias " + group.id;
        rows.push_back({"L2", "resource", line, {}, resource.name});
    }
    for (const auto &group : plan_alias_groups_)
        if (meets(group.from_nodes, nodes))
            rows.push_back(
                {"L2", "alias", "alias group " + group.id + ": " + joinedNames(group.resources), {}});
    for (const auto &scope : plan_scopes_)
        if (meets(scope.nodes, nodes))
            rows.push_back({"L2", "scope", "scope " + scope.id + " (" + scope.kind + ")", {}});
    for (const auto &barrier : plan_barriers_) {
        if (!listed(nodes, barrier.from) && !listed(nodes, barrier.to)) continue;
        auto line = barrier.from + arrow + barrier.to + dot + barrier.resource;
        if (!barrier.kind.empty()) line += dot + barrier.kind;
        const auto access = [&](const std::string &node) -> std::string {
            const auto uses = plan_access_.find(node);
            if (uses == plan_access_.end()) return {};
            const auto use = uses->second.find(barrier.resource);
            return use == uses->second.end() ? std::string{} : use->second;
        };
        if (const auto from = access(barrier.from), to = access(barrier.to);
            !from.empty() && !to.empty())
            line += dot + from + arrow + to;
        rows.push_back({"L2", "barrier", line, {}});
    }
    // L3: the decisions about it, and a backend other than the graph's.
    for (const auto &decision : plan_decisions_)
        if (meets(decision.from_nodes, nodes))
            rows.push_back({"L3", "decision",
                            decision.id + dot + decision.subject + arrow + decision.selected, {}});
    const auto graph = graphBackend();
    for (const auto &name : nodes) {
        const auto *pass = planPass(name);
        if (!pass->backend.empty() && pass->backend != graph)
            rows.push_back({"L3", "backend", "backend " + pass->backend + " (" + name + ")", name});
    }
    // RTL-2s-c (RTL-2s decision 4): a ray tracing task's shader binding
    // table as its ray layout lays it out -- the hit groups (one record per
    // group and ray type), the ray types it traces, its misses -- and the
    // acceleration structure its rays are traced against.
    if (const auto *node = findNode({}, canvas_node); node != nullptr && text(*node, "op") == "render.compute") {
        const auto parameters = node->value("parameters", Json::object());
        const auto rt = parameters.is_object() ? parameters.value("ray_tracing", Json(nullptr)) : Json(nullptr);
        if (rt.is_object() && ray_layout_read_ && ray_layout_read_->layout.usable() && rayTracingUsesLayout(rt)) {
            const auto &layout = ray_layout_read_->layout.document;
            const auto join = [](const std::vector<std::string> &names) {
                std::string line;
                for (const auto &name : names) line += (line.empty() ? "" : ", ") + name;
                return line;
            };
            std::vector<std::string> groups;
            for (const auto &group : rayLayoutGroups(layout))
                groups.push_back(group.name + (group.type == "procedural" ? " (procedural)" : "") +
                                 (group.name == rayLayoutDefaultGroup(layout) ? " (default)" : ""));
            std::vector<std::string> traced;
            for (const auto &type : rt.value("traced_ray_types", Json::array()))
                if (type.is_string()) traced.push_back(type.get<std::string>());
            rows.push_back({"L3", "ray_layout", "hit groups " + join(groups), {}});
            rows.push_back({"L3", "ray_layout",
                            "ray types traced " + join(traced) + " of " + join(rayLayoutRayTypes(layout)), {}});
            rows.push_back({"L3", "ray_layout", "misses " + join(rayLayoutMisses(layout)), {}});
            const auto structure = rt.value("acceleration_structure", Json(nullptr));
            rows.push_back({"L3", "ray_layout",
                            "acceleration structure " +
                                (structure.is_string() ? structure.get<std::string>()
                                 : structure.is_null() ? std::string{"scene"}
                                                       : structure.dump()),
                            {}});
        }
    }
    return rows;
}

std::vector<GraphCanvasLayerRow> GraphCanvasModel::graphLayerRows() const {
    std::vector<GraphCanvasLayerRow> rows;
    if (!layersShown()) return rows;
    const std::string dot{kDot};
    // UX-15b, decision 5 (GL-2b's hole): a feature whose passes graph nodes
    // took the place of, every one, has no folded node to carry its
    // "is replaced by" rows (layerRows). The graph's own rows carry them.
    const auto folded = rootFoldedNodes();
    std::vector<std::string> said;
    for (const auto &pass : plan_passes_)
        for (const auto &replaced : pass.replaces) {
            const bool on_feature =
                std::ranges::any_of(folded, [&](const GraphCanvasFoldedNode &node) {
                    return node.feature_group && (node.feature == replaced.feature ||
                                                  featureShortName(node.feature) == replaced.feature);
                });
            if (on_feature) continue;
            auto owner = graphOwnerOf(pass);
            if (owner.empty()) owner = pass.name;
            const auto line = "pass " + replaced.pass + " of " + replaced.feature +
                              " is replaced by graph node " + owner;
            if (listed(said, line)) continue;
            said.push_back(line);
            rows.push_back({"L1", "replaced", line, pass.name});
        }
    for (const auto &decision : plan_decisions_)
        if (decision.from_nodes.empty())
            rows.push_back({"L3", "decision",
                            decision.id + dot + decision.subject + std::string{kArrow} +
                                decision.selected,
                            {}});
    for (const auto &candidate : plan_candidates_)
        rows.push_back({"L3", "backend",
                        "candidate " + candidate.candidate +
                            (candidate.endpoint.empty() ? std::string{} : dot + candidate.endpoint) +
                            dot +
                            (candidate.selected   ? "selected"
                             : candidate.feasible ? "feasible"
                                                  : "rejected"),
                        {},
                        candidate.candidate});
    return rows;
}

bool GraphCanvasModel::foldedDocument(const GraphCanvasFoldedStep &step, Json &nodes,
                                      Json &definitions, Json &reports) const {
    reports = Json::object();
    definitions = Json::object();
    nodes = Json::array();
    if (step.kind == "feature")
        return std::ranges::any_of(plan_passes_, [&](const GraphCanvasPlanPass &pass) {
            return pass.feature == step.feature;
        });
    if (step.kind == "decomposed") {
        const auto found = templates_.find(step.feature);
        if (found == templates_.end() || !found->is_object()) return false;
        if (const auto held = found->find("nodes"); held != found->end() && held->is_array())
            nodes = *held;
        if (const auto held = found->find("definitions");
            held != found->end() && held->is_object())
            definitions = *held;
        return true;
    }
    if (step.kind != "pass") return false;
    const auto *pass = planPass(step.id);
    if (pass == nullptr || !pass->material) return false;
    // design_graph_loops 4.3: a material pass means a render.material_pass
    // taking the draws of an else-only draw_loop over its materials, whose
    // body draws each material (or its variant). Studio builds that here, as
    // decompose_material_pass would write it, and never writes it anywhere.
    const auto loop = pass->name + "_draws";
    const auto body = pass->name + "_draw";
    Json step_node{{"id", "step"}, {"outputs", Json::array({"draws"})}};
    if (pass->material_variant.empty()) {
        step_node["op"] = "draw.material";
        step_node["parameters"] = Json::object();
    } else {
        step_node["op"] = "draw.variant";
        step_node["parameters"] = Json{{"variant", pass->material_variant}};
    }
    Json draw_items = Json::object();
    if (!pass->contract.empty()) draw_items["contract"] = pass->contract;
    Json branch = Json::object();
    branch["body"] = body;
    Json loop_parameters = Json::object();
    loop_parameters["over"] = Json{{"draw_items", draw_items}};
    loop_parameters["tier"] = "draw_loop";
    loop_parameters["branches"] = Json::array({branch});
    Json loop_node{{"id", loop}, {"op", "graph.loop"}, {"outputs", Json::array({"draws"})}};
    loop_node["parameters"] = std::move(loop_parameters);
    Json pass_parameters = Json::object();
    if (!pass->contract.empty()) pass_parameters["contract"] = pass->contract;
    if (!pass->material_variant.empty()) pass_parameters["material_variant"] = pass->material_variant;
    Json draws_edge = Json::object();
    draws_edge["node"] = loop;
    draws_edge["port"] = "draws";
    Json pass_inputs = Json::object();
    pass_inputs["draws"] = draws_edge;
    Json pass_node{{"id", pass->name}, {"op", std::string{kMaterialPass}}, {"outputs", Json::array()}};
    pass_node["parameters"] = std::move(pass_parameters);
    pass_node["inputs"] = std::move(pass_inputs);
    nodes = Json::array({std::move(loop_node), std::move(pass_node)});
    Json body_output = Json::object();
    body_output["node"] = "step";
    body_output["port"] = "draws";
    Json body_outputs = Json::object();
    body_outputs["draws"] = body_output;
    Json body_definition = Json::object();
    body_definition["nodes"] = Json::array({std::move(step_node)});
    body_definition["outputs"] = std::move(body_outputs);
    definitions[body] = std::move(body_definition);
    // No loops[] entry names it (the engine runs no loop here): the zone's
    // stage is the draw_loop's, with the plan's count when it resolved one.
    Json report{{"node", loop}, {"tier", "draw_loop"}, {"pass", pass->name},
                {"elements", Json::array()}, {"unmatched_branches", Json::array()}};
    if (pass->draw_count) {
        report["count"] = *pass->draw_count;
        report["total"] = *pass->draw_count;
    }
    reports[loop] = std::move(report);
    return true;
}

bool GraphCanvasModel::showFoldedTop() {
    if (folded_trail_.empty()) return false;
    Json nodes;
    Json definitions;
    Json reports;
    if (!foldedDocument(folded_trail_.back(), nodes, definitions, reports)) return false;
    if (!folded_stash_)
        folded_stash_ = FoldedStash{nodes_,         definitions_,           layout_,
                                    loop_reports_,  loop_reports_received_, breadcrumb_,
                                    entered_loop_,  loop_branch_};
    nodes_ = std::move(nodes);
    definitions_ = std::move(definitions);
    layout_ = Json::object();
    loop_reports_ = std::move(reports);
    loop_reports_received_ = !loop_reports_.empty();
    breadcrumb_ = {std::string{}};
    entered_loop_.clear();
    loop_branch_.clear();
    ++content_revision_;
    ++report_revision_;
    return true;
}

void GraphCanvasModel::restoreFoldedStash() {
    if (!folded_stash_) return;
    auto stash = std::move(*folded_stash_);
    folded_stash_.reset();
    nodes_ = std::move(stash.nodes);
    definitions_ = std::move(stash.definitions);
    layout_ = std::move(stash.layout);
    loop_reports_ = std::move(stash.loop_reports);
    loop_reports_received_ = stash.loop_reports_received;
    breadcrumb_ = std::move(stash.breadcrumb);
    entered_loop_ = std::move(stash.entered_loop);
    loop_branch_ = std::move(stash.loop_branch);
}

void GraphCanvasModel::leaveFolded() {
    folded_refused_ = false;
    if (!folded_stash_) {
        folded_trail_.clear();
        return;
    }
    restoreFoldedStash();
    folded_trail_.clear();
    ++content_revision_;
    ++report_revision_;
}

bool GraphCanvasModel::enterFolded(std::string_view id) {
    if (!currentScope().empty()) return false;
    const auto nodes = foldedNodes();
    const auto found = std::ranges::find(nodes, id, &GraphCanvasFoldedNode::id);
    if (found == nodes.end() || !found->enterable()) return false;
    folded_trail_.push_back(GraphCanvasFoldedStep{found->feature_group ? "feature" : "pass",
                                                  found->id, found->label, found->feature});
    if (!showFoldedTop()) {
        folded_trail_.pop_back();
        return false;
    }
    folded_refused_ = false;
    enteredScope();
    return true;
}

bool GraphCanvasModel::canShowDecomposed() const {
    return folded_stash_ && currentScope().empty() && !folded_trail_.empty() &&
           folded_trail_.back().kind == "feature" &&
           templates_.contains(folded_trail_.back().feature);
}

bool GraphCanvasModel::showDecomposed() {
    if (!canShowDecomposed()) return false;
    const auto &feature = folded_trail_.back();
    folded_trail_.push_back(GraphCanvasFoldedStep{"decomposed", feature.id,
                                                  feature.label + " (decomposed)", feature.feature});
    if (!showFoldedTop()) {
        folded_trail_.pop_back();
        return false;
    }
    folded_refused_ = false;
    enteredScope();
    return true;
}

std::vector<std::string> GraphCanvasModel::displayTrail() const {
    std::vector<std::string> trail{"root"};
    if (folded_stash_)
        for (const auto &step : folded_trail_) trail.push_back(step.label);
    for (std::size_t index = 1; index < breadcrumb_.size(); ++index)
        trail.push_back(breadcrumb_[index]);
    return trail;
}

std::string GraphCanvasModel::viewKey() const {
    if (!folded_stash_) return currentScope();
    std::string key = "\x1f";
    for (const auto &step : folded_trail_) key += step.kind + ":" + step.id + "/";
    return key + currentScope();
}

GraphCanvasFoldedExpand GraphCanvasModel::foldedExpand() const {
    GraphCanvasFoldedExpand expand;
    if (!folded_stash_) return expand;
    for (auto step = folded_trail_.rbegin(); step != folded_trail_.rend(); ++step) {
        // A material pass of the rendering section is decomposed; one a
        // feature made is expanded with its feature.
        if (step->kind == "pass" && step->feature.empty()) {
            expand.kind = "decompose";
            expand.graph = plan_graph_;
            expand.pass = step->id;
            return expand;
        }
        if ((step->kind == "feature" || step->kind == "decomposed") &&
            templates_.contains(step->feature)) {
            expand.kind = "replace";
            expand.feature = step->feature;
            return expand;
        }
    }
    return expand;
}

GraphCanvasFoldedExpand GraphCanvasModel::foldedNodeExpand(std::string_view id) const {
    GraphCanvasFoldedExpand expand;
    if (folded_stash_ || !currentScope().empty()) return expand;
    for (const auto &node : foldedNodes()) {
        if (node.id != id) continue;
        // As foldedExpand reads the trail: a material pass is its own step
        // when no feature made it, else its feature's.
        if (node.material && !node.feature_group && node.feature.empty()) {
            expand.kind = "decompose";
            expand.graph = plan_graph_;
            expand.pass = node.id;
        } else if (!node.feature.empty() && templates_.contains(node.feature)) {
            expand.kind = "replace";
            expand.feature = node.feature;
        }
        break;
    }
    return expand;
}

bool GraphCanvasModel::refuseFoldedEdit(const Json &ops) {
    // A drag inside the view moves nothing that is kept, and says nothing.
    const bool placement = ops.is_array() && std::ranges::all_of(ops, [](const Json &op) {
        return op.is_object() && text(op, "op") == "set_layout";
    });
    if (!placement) {
        folded_refused_ = true;
        ++report_revision_;
    }
    return false;
}

const std::vector<std::string> &GraphCanvasModel::domainNames() {
    static const std::vector<std::string> names{"render", "pose", "value", "audio", "ui"};
    return names;
}

std::string GraphCanvasModel::opDomain(std::string_view op) {
    const auto dot = op.find('.');
    if (dot == std::string_view::npos) return {};
    const auto prefix = op.substr(0, dot);
    // design_graph_layers 3.3: draw.* is drawn with render.*, and event.*
    // is shown with the values it drives.
    if (prefix == "render" || prefix == "draw") return "render";
    if (prefix == "value" || prefix == "event") return "value";
    if (prefix == "pose" || prefix == "audio" || prefix == "ui") return std::string{prefix};
    return {};
}

bool GraphCanvasModel::domainShown(std::string_view domain) const {
    return domain.empty() || domains_shown_.contains(domain);
}

bool GraphCanvasModel::setDomainShown(const std::string &domain, bool shown) {
    if (!listed(domainNames(), domain) || domains_shown_.contains(domain) == shown) return false;
    if (shown) domains_shown_.insert(domain);
    else domains_shown_.erase(domain);
    ++report_revision_;
    return true;
}

bool GraphCanvasModel::nodeShown(std::string_view scope, const GraphCanvasNode &node) const {
    // A graph.instance and a graph.loop are structure: whatever domain their
    // body is in, they stay (opDomain answers nothing for graph.*).
    if (!scope.empty() || folded_stash_) return true;
    return domainShown(opDomain(node.op));
}

std::vector<GraphCanvasDomainStub> GraphCanvasModel::domainStubs() const {
    std::vector<GraphCanvasDomainStub> result;
    if (folded_stash_ || !currentScope().empty()) return result;
    const auto drawn = nodes({});
    std::map<std::string, std::string, std::less<>> domain_of;
    std::set<std::string, std::less<>> hidden;
    for (const auto &node : drawn) {
        domain_of[node.id] = opDomain(node.op);
        if (!nodeShown({}, node)) hidden.insert(node.id);
    }
    const auto all = edges({});
    struct Cut {
        GraphCanvasDomainStub stub;
        std::set<std::string, std::less<>> nodes;
    };
    std::vector<Cut> cuts;
    const auto cut = [&](const std::string &node, const std::string &port, bool input,
                         const std::string &domain) -> Cut & {
        const auto found = std::ranges::find_if(cuts, [&](const Cut &entry) {
            return entry.stub.node == node && entry.stub.port == port &&
                   entry.stub.input == input && entry.stub.domain == domain;
        });
        if (found != cuts.end()) return *found;
        cuts.push_back(Cut{GraphCanvasDomainStub{node, port, input, domain, 0}, {}});
        return cuts.back();
    };
    for (const auto &edge : all) {
        const bool from_hidden = hidden.contains(edge.from_node);
        const bool to_hidden = hidden.contains(edge.to_node);
        if (from_hidden == to_hidden) continue;
        if (from_hidden) {
            // The hidden producer and the hidden nodes of its domain it
            // reads from, however far up.
            const auto domain = domain_of[edge.from_node];
            auto &entry = cut(edge.to_node, edge.to_port, true, domain);
            std::vector<std::string> stack{edge.from_node};
            std::set<std::string, std::less<>> seen;
            while (!stack.empty()) {
                auto id = std::move(stack.back());
                stack.pop_back();
                if (!seen.insert(id).second) continue;
                if (domain_of[id] == domain) entry.nodes.insert(id);
                for (const auto &upstream : all)
                    if (upstream.to_node == id && hidden.contains(upstream.from_node))
                        stack.push_back(upstream.from_node);
            }
        } else {
            cut(edge.from_node, edge.from_port, false, domain_of[edge.to_node])
                .nodes.insert(edge.to_node);
        }
    }
    for (auto &entry : cuts) {
        entry.stub.count = entry.nodes.size();
        result.push_back(std::move(entry.stub));
    }
    return result;
}

// --- UX-2: a node's values chosen rather than typed.

bool GraphCanvasModel::setNodeParameters(std::string_view scope, const std::string &node,
                                         const Json &parameters) {
    if (!canEdit() || findNode(scope, node) == nullptr || !parameters.is_object()) return false;
    if (parameters == nodeParameters(scope, node)) return false;
    return submitOps(Json::array({Json{
        {"op", "set_node_parameters"}, {"scope", scopeToken(scope)}, {"node", node},
        {"parameters", parameters}}}));
}

namespace {

std::vector<std::string> vocabularyList(const Json &context, const char *field) {
    std::vector<std::string> values;
    const auto vocabulary = context.find("render_target_vocabulary");
    if (vocabulary == context.end() || !vocabulary->is_object()) return values;
    const auto list = vocabulary->find(field);
    if (list == vocabulary->end() || !list->is_array()) return values;
    for (const auto &value : *list)
        if (value.is_string()) values.push_back(value.get<std::string>());
    return values;
}

bool hasTrait(const Json &schema, std::string_view trait) {
    const auto traits = schema.find("traits");
    return traits != schema.end() && traits->is_array() &&
           std::ranges::any_of(*traits, [&](const Json &entry) {
               return entry.is_string() && entry.get<std::string>() == trait;
           });
}

// `preferred` when the engine's list has it (or there is no list), else the
// list's first format on the same side of the depth line; empty when none is.
std::string pickFormat(const GraphCanvasModel &model, const std::vector<std::string> &formats,
                       std::string_view preferred, bool depth) {
    if (formats.empty() || std::ranges::find(formats, preferred) != formats.end())
        return std::string{preferred};
    for (const auto &format : formats)
        if (model.isDepthFormat(format) == depth) return format;
    return {};
}

} // namespace

bool GraphCanvasModel::renderTargetVocabularyKnown() const {
    return !vocabularyList(render_authoring_, "formats").empty();
}

std::vector<std::string> GraphCanvasModel::renderTargetFormats() const {
    return vocabularyList(render_authoring_, "formats");
}

std::vector<std::string> GraphCanvasModel::renderTargetUsages() const {
    return vocabularyList(render_authoring_, "usage");
}

void GraphCanvasModel::askRenderTargetVocabulary() {
    if (!connected_ || vocabulary_asked_ || renderTargetVocabularyKnown()) return;
    // A context that came without it is the engine's answer already.
    if (!render_authoring_.empty()) return;
    // The plan's own request may be on its way: its reply serves both.
    if (std::ranges::any_of(reads_, [](const PendingRequest &pending) {
            return pending.kind == RequestKind::Authoring;
        }))
        return;
    vocabulary_asked_ = true;
    read(RequestKind::Authoring, "get_render_authoring_context", Json::object());
}

void GraphCanvasModel::readRenderAuthoringAgain() {
    if (!connected_) return;
    read(RequestKind::Authoring, "get_render_authoring_context", Json::object());
}

std::string GraphCanvasModel::semanticLabel(std::string_view semantic) {
    auto name = semantic;
    if (const auto at = name.find('@'); at != std::string_view::npos) name = name.substr(0, at);
    if (const auto dot = name.rfind('.'); dot != std::string_view::npos) name = name.substr(dot + 1);
    std::string label{name};
    std::ranges::replace(label, '_', ' ');
    if (label.empty()) return std::string{semantic};
    label.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(label.front())));
    return label;
}

bool GraphCanvasModel::depthFormat(std::string_view format) {
    return format.size() > 2 && format.front() == 'D' &&
           std::isdigit(static_cast<unsigned char>(format[1])) != 0;
}

std::vector<GraphCanvasImageType> GraphCanvasModel::imageTypes() const {
    std::vector<GraphCanvasImageType> types;
    std::vector<GraphCanvasImageType> legacy;
    for (const auto &[id, schema] : types_.items()) {
        if (!schema.is_object() || text(schema, "constructor") != "image") continue;
        GraphCanvasImageType type;
        type.semantic = id;
        type.label = semanticLabel(id);
        type.depth = hasTrait(schema, "depth_like");
        type.legacy = hasTrait(schema, "legacy_untyped");
        for (const auto &parameter : schema.value("parameters", Json::array())) {
            if (!parameter.is_object()) continue;
            GraphCanvasTypeArgument argument;
            argument.name = text(parameter, "name");
            if (argument.name.empty()) continue;
            for (const auto &value : parameter.value("allowed", Json::array()))
                if (value.is_string()) argument.allowed.push_back(value.get<std::string>());
            // The engine writes a default as {"kind": "enum_value", "value": ...}.
            if (const auto fallback = parameter.find("default"); fallback != parameter.end()) {
                if (fallback->is_string())
                    argument.default_value = fallback->get<std::string>();
                else if (fallback->is_object())
                    argument.default_value = text(*fallback, "value");
            }
            type.arguments.push_back(std::move(argument));
        }
        // EL-1: `arguments: {<name>: {kind, required, values: [...],
        // default} | {min, max}}`, the engine parser's own table, says what
        // each argument takes; where it is there it is the list. A {min, max}
        // one is not a list: the field shows the document's value.
        if (const auto declared = schema.find("arguments");
            declared != schema.end() && declared->is_object())
            for (const auto &[name, range] : declared->items()) {
                if (!range.is_object()) continue;
                auto found = std::ranges::find(type.arguments, name, &GraphCanvasTypeArgument::name);
                if (found == type.arguments.end()) {
                    type.arguments.push_back(GraphCanvasTypeArgument{name, {}, {}});
                    found = std::prev(type.arguments.end());
                }
                if (const auto fallback = range.find("default"); fallback != range.end()) {
                    if (fallback->is_string()) found->default_value = fallback->get<std::string>();
                    else if (fallback->is_object()) found->default_value = text(*fallback, "value");
                }
                const auto values = range.find("values");
                if (values == range.end() || !values->is_array()) continue;
                found->allowed.clear();
                for (const auto &value : *values)
                    if (value.is_string()) found->allowed.push_back(value.get<std::string>());
            }
        (type.legacy ? legacy : types).push_back(std::move(type));
    }
    std::ranges::move(legacy, std::back_inserter(types));
    return types;
}

std::optional<GraphCanvasImageType> GraphCanvasModel::imageType(std::string_view semantic) const {
    for (auto &type : imageTypes())
        if (type.semantic == semantic) return std::move(type);
    return std::nullopt;
}

std::vector<std::string> GraphCanvasModel::imageFormatChoices(const Json &parameters) const {
    auto formats = renderTargetFormats();
    if (formats.empty()) return formats;
    const auto type = parameters.is_object() ? parameters.value("type", Json::object()) : Json::object();
    const auto semantic = type.is_object() ? text(type, "semantic") : std::string{};
    const auto known = semantic.empty() ? std::nullopt : imageType(semantic);
    // A type the catalog does not name narrows nothing.
    if (!known) return formats;
    // UX-5b, decision 4: which side of the depth line a format is on is the
    // engine's format_aspects, not its name.
    std::erase_if(formats,
                  [&](const std::string &format) { return isDepthFormat(format) != known->depth; });
    return formats;
}

Json GraphCanvasModel::imageParametersWithSemantic(const Json &parameters,
                                                   std::string_view semantic) const {
    Json result = parameters.is_object() ? parameters : Json::object();
    const auto target = imageType(semantic);
    if (!target) return result;
    const auto old_type = result.value("type", Json::object());
    const auto old_arguments =
        old_type.is_object() ? old_type.value("arguments", Json::object()) : Json::object();
    Json arguments = Json::object();
    for (const auto &argument : target->arguments) {
        if (old_arguments.is_object()) {
            const auto kept = old_arguments.find(argument.name);
            if (kept != old_arguments.end() && kept->is_string() &&
                (argument.allowed.empty() ||
                 std::ranges::find(argument.allowed, kept->get<std::string>()) !=
                     argument.allowed.end())) {
                arguments[argument.name] = *kept;
                continue;
            }
        }
        if (!argument.default_value.empty()) continue;
        if (!argument.allowed.empty()) arguments[argument.name] = argument.allowed.front();
    }
    result["type"] = Json{{"semantic", std::string{semantic}}, {"arguments", std::move(arguments)}};
    // Across the depth line the format and the attachment flag follow.
    const auto format = result.value("format", std::string{});
    if (!format.empty() && isDepthFormat(format) != target->depth) {
        const auto picked = pickFormat(*this, renderTargetFormats(),
                                       target->depth ? "D32_SFLOAT" : "R16G16B16A16_SFLOAT",
                                       target->depth);
        if (!picked.empty()) result["format"] = picked;
        if (const auto usage = result.find("usage"); usage != result.end() && usage->is_array()) {
            const std::string from = target->depth ? "COLOR_ATTACHMENT" : "DEPTH_STENCIL_ATTACHMENT";
            const std::string to = target->depth ? "DEPTH_STENCIL_ATTACHMENT" : "COLOR_ATTACHMENT";
            for (auto &flag : *usage)
                if (flag.is_string() && flag.get<std::string>() == from) flag = to;
        }
    }
    return result;
}

void GraphCanvasModel::setShaderChoices(std::vector<std::string> vertex,
                                        std::vector<std::string> fragment) {
    vertex_shader_choices_ = std::move(vertex);
    fragment_shader_choices_ = std::move(fragment);
}

std::vector<GraphCanvasValueChoice> GraphCanvasModel::valueChoices(std::string_view scope,
                                                                   std::string_view node,
                                                                   std::string_view port) const {
    std::vector<GraphCanvasValueChoice> choices;
    const auto *found = findNode(scope, node);
    if (found == nullptr) return choices;
    const auto op = text(*found, "op");
    const auto parameters = nodeParameters(scope, node);
    if (op == "render.image" && port == "format") {
        for (const auto &format : imageFormatChoicesFor(scope, node)) {
            auto changed = parameters;
            changed["format"] = format;
            choices.push_back({format, std::move(changed)});
        }
    } else if (op == "render.image" && port == "type") {
        const auto current = parameters.value("type", Json::object());
        const auto semantic = current.is_object() ? text(current, "semantic") : std::string{};
        for (const auto &type : imageTypes()) {
            if (type.legacy && type.semantic != semantic) continue;
            // The type on the node stays as it is; another one comes with the
            // arguments imageParametersWithSemantic gives it.
            choices.push_back({type.label, type.semantic == semantic
                                               ? parameters
                                               : imageParametersWithSemantic(parameters, type.semantic)});
        }
    } else if ((op == "render.fullscreen" || op == "render.raster") && port == "shader") {
        const auto shader = parameters.value("shader", Json::object());
        if (!shader.is_object() || !shader.contains("fragment")) return choices;
        for (const auto &reference : fragment_shader_choices_) {
            auto changed = parameters;
            changed["shader"]["fragment"] = reference;
            choices.push_back({reference, std::move(changed)});
        }
    }
    return choices;
}

// --- UX-3: the palette's other render nodes, a render node's place, its
// ports' settings, and the engine's lists (EL-1) where names were typed.

namespace {

// The render nodes the Place field is shown for.
constexpr std::array<std::string_view, 6> kPlacedRenderOps{
    "render.fullscreen", "render.raster", "render.material_pass",
    "render.shadow_depth", "render.snapshot_copy", "render.compute"};

std::vector<std::string> stringsAt(const Json &object, const char *key) {
    std::vector<std::string> values;
    if (!object.is_object()) return values;
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array()) return values;
    for (const auto &value : *found)
        if (value.is_string()) values.push_back(value.get<std::string>());
    return values;
}

// The keys of a render.image (or a target declaration) that say its size.
void copyImageSize(const Json &from, Json &to) {
    if (from.is_object())
        for (const char *key : {"extent_scale", "width", "height", "layers"})
            if (const auto found = from.find(key); found != from.end() && found->is_number())
                to[key] = *found;
    if (!to.contains("extent_scale")) to["extent_scale"] = 1.0;
}

} // namespace

Json GraphCanvasModel::targetRefType(const Json &declaration) {
    if (declaration.is_object())
        if (const auto type = declaration.find("type");
            type != declaration.end() && type->is_object() && !text(*type, "semantic").empty())
            return *type;
    std::string topology;
    if (declaration.is_object()) {
        if (text(declaration, "dimension") == "cube") topology = "cube";
        else if (const auto layers = declaration.find("layers");
                 layers != declaration.end() && layers->is_number_integer() &&
                 layers->get<std::int64_t>() > 1)
            topology = "two_d_array";
    }
    // A depth format is a depth image; anything else (or no format yet) is
    // a scene colour, the type a render pass's ports carry.
    const bool depth = declaration.is_object() && depthFormat(text(declaration, "format"));
    Json type = depth ? Json{{"semantic", "pelican.render.depth@1"},
                             {"arguments", Json{{"representation", "device"}, {"space", "projection"}}}}
                      : sceneColorType();
    if (!topology.empty()) type["arguments"]["topology"] = topology;
    return type;
}

std::vector<GraphCanvasDeclaredTarget> GraphCanvasModel::declaredRenderTargets() const {
    std::vector<GraphCanvasDeclaredTarget> targets;
    const auto found = render_authoring_.find("render_targets");
    if (found == render_authoring_.end() || !found->is_array()) return targets;
    for (const auto &entry : *found) {
        if (!entry.is_object()) continue;
        GraphCanvasDeclaredTarget target;
        target.name = text(entry, "name");
        if (target.name.empty()) continue;
        // The graph's own images are read by their node, not by name.
        if (const auto *node = findNode({}, target.name);
            node != nullptr && text(*node, "op") == "render.image")
            continue;
        if (std::ranges::find(targets, target.name, &GraphCanvasDeclaredTarget::name) != targets.end())
            continue;
        target.source = text(entry, "source");
        target.declaration = entry.value("declaration", Json::object());
        target.type = targetRefType(target.declaration);
        target.depth = text(target.type, "semantic") == "pelican.render.depth@1";
        targets.push_back(std::move(target));
    }
    return targets;
}

std::vector<std::pair<std::string, std::string>>
GraphCanvasModel::materialPassInputs(std::string_view kind) const {
    std::vector<std::pair<std::string, std::string>> inputs;
    for (const auto &contract : material_pass_contracts_) {
        if (!contract.is_object()) continue;
        const auto found = contract.find(std::string{kind});
        if (found == contract.end() || !found->is_array()) continue;
        for (const auto &entry : *found) {
            const auto name = text(entry, "name");
            if (name.empty() || std::ranges::find(inputs, name, &std::pair<std::string, std::string>::first) !=
                                    inputs.end())
                continue;
            inputs.emplace_back(name, text(entry, "source_type"));
        }
    }
    return inputs;
}

void GraphCanvasModel::followDrawnMaterials() {
    if (std::ranges::any_of(plan_passes_, [](const GraphCanvasPlanPass &pass) {
            return pass.drawn_materials.has_value();
        }))
        return;
    drawn_follow_reads_ = drawnFollowLimit;
}

std::string GraphCanvasModel::targetSemantic(std::string_view target) const {
    if (target.empty()) return {};
    for (const auto &node : nodes_) {
        if (!node.is_object()) continue;
        const auto op = text(node, "op");
        if (op != "render.image" && op != "render.target_ref") continue;
        const auto parameters = node.value("parameters", Json::object());
        if (!parameters.is_object()) continue;
        const bool named = text(parameters, "name") == target ||
                           (op == "render.image" && text(node, "id") == target);
        if (!named) continue;
        if (const auto type = parameters.find("type"); type != parameters.end() && type->is_object())
            if (auto semantic = text(*type, "semantic"); !semantic.empty()) return semantic;
    }
    if (const auto found = render_authoring_.find("render_targets");
        found != render_authoring_.end() && found->is_array())
        for (const auto &entry : *found)
            if (entry.is_object() && text(entry, "name") == target)
                return text(targetRefType(entry.value("declaration", Json::object())), "semantic");
    return {};
}

void GraphCanvasModel::setComputeShaderChoices(std::vector<std::string> choices) {
    compute_shader_choices_ = std::move(choices);
}

bool GraphCanvasModel::rasterTemplateAvailable() const {
    return std::ranges::find(vertex_shader_choices_, std::string{"engine://fullscreen"}) !=
               vertex_shader_choices_.end() &&
           std::ranges::find(fragment_shader_choices_, std::string{"engine://passthrough"}) !=
               fragment_shader_choices_.end();
}

const std::vector<std::string> &GraphCanvasModel::bufferElements() {
    // shaderresourceport's element names, in the combo's order.
    static const std::vector<std::string> elements{"float", "vec2",  "vec3",  "vec4", "int",
                                                   "ivec2", "ivec3", "ivec4", "uint", "uvec2",
                                                   "uvec3", "uvec4", "mat4"};
    return elements;
}

const std::vector<std::string> &GraphCanvasModel::attributeDomains() {
    static const std::vector<std::string> domains{"vertex", "instance", "index"};
    return domains;
}

std::uint64_t GraphCanvasModel::bufferElementSize(std::string_view element) {
    // shaderResourceBufferElementStride, restated: a three-wide element takes
    // the room of four (std430).
    if (element == "mat4") return 64;
    if (element.ends_with("vec2")) return 8;
    if (element.ends_with("vec3") || element.ends_with("vec4")) return 16;
    return 4;
}

void GraphCanvasModel::snapshotCopySeedOps(std::string_view in_scope, const std::string &scope_token,
                                           const std::string &id, const std::string &after,
                                           Json &ops, Json &inputs) const {
    const auto add = [&](Json node) {
        ops.push_back(Json{{"op", "add_node"}, {"scope", scope_token}, {"node", std::move(node)}});
    };
    // What the copy reads is what the pass it follows wrote: the image the
    // copy's own image is shaped after.
    Json source_image = Json::object();
    Json source_type;
    const auto targets = declaredRenderTargets();
    const auto readImageNode = [&](const std::string &resource) {
        const auto *image = findNode({}, resource);
        if (image == nullptr || text(*image, "op") != "render.image") return false;
        source_image = image->value("parameters", Json::object());
        source_type = source_image.value("type", Json{});
        return true;
    };
    const auto readTarget = [&](const std::string &resource) -> const GraphCanvasDeclaredTarget * {
        const auto found = std::ranges::find(targets, resource, &GraphCanvasDeclaredTarget::name);
        if (found == targets.end()) return nullptr;
        source_image = found->declaration;
        source_type = found->type;
        return &*found;
    };
    bool have_source = false;
    if (in_scope.empty()) {
        // A pass a root node made: its first output that hands on an image.
        if (const auto *writer = findNode({}, after); writer != nullptr)
            for (const auto &output : writer->value("outputs", Json::array())) {
                if (!output.is_string()) continue;
                const auto port = output.get<std::string>();
                const auto resource = outputResource(after, port);
                if (resource.empty()) continue;
                if (!readImageNode(resource)) readTarget(resource);
                inputs["source"] = Json{{"node", after}, {"port", port}};
                have_source = true;
                break;
            }
        // A pass of the rendering section, the preset or a feature: a target
        // it writes, read by its node or by name.
        const auto pass = std::ranges::find(plan_passes_, after, &GraphCanvasPlanPass::name);
        if (!have_source && pass != plan_passes_.end())
            for (const auto &written : pass->writes) {
                if (readImageNode(written)) {
                    inputs["source"] = Json{{"node", written}, {"port", "resource"}};
                    have_source = true;
                    break;
                }
                const auto *target = readTarget(written);
                if (target == nullptr) continue;
                std::string reference;
                for (const auto &node : nodes_)
                    if (node.is_object() && text(node, "op") == "render.target_ref" &&
                        text(node.value("parameters", Json::object()), "name") == written)
                        reference = text(node, "id");
                if (reference.empty()) {
                    reference = written + "_ref";
                    for (int suffix = 2; findNode({}, reference) != nullptr && suffix < 10000; ++suffix)
                        reference = written + "_ref_" + std::to_string(suffix);
                    add(Json{{"id", reference}, {"op", "render.target_ref"},
                             {"parameters", Json{{"name", written}, {"type", target->type}}},
                             {"outputs", Json::array({"resource"})}});
                }
                inputs["source"] = Json{{"node", reference}, {"port", "resource"}};
                have_source = true;
                break;
            }
    }
    if (!have_source) {
        // Nothing known to copy: an image of its own, which a pass can write.
        Json parameters{{"type", sceneColorType()}, {"format", "R16G16B16A16_SFLOAT"},
                        {"extent_scale", 1.0},
                        {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"})}};
        source_image = parameters;
        source_type = sceneColorType();
        add(Json{{"id", id + "__source"}, {"op", "render.image"},
                 {"parameters", std::move(parameters)}, {"outputs", Json::array({"resource"})}});
        inputs["source"] = Json{{"node", id + "__source"}, {"port", "resource"}};
    }
    // The copy: the source's type, format and size, written by a transfer.
    auto format = text(source_image, "format");
    if (format.empty()) format = "R16G16B16A16_SFLOAT";
    Json destination{{"type", source_type.is_object() ? source_type : sceneColorType()},
                     {"format", format},
                     {"usage", Json::array({"TRANSFER_DST", "SAMPLED"})}};
    copyImageSize(source_image, destination);
    add(Json{{"id", id + "__copy"}, {"op", "render.image"}, {"parameters", std::move(destination)},
             {"outputs", Json::array({"resource"})}});
    inputs["destination"] = Json{{"node", id + "__copy"}, {"port", "resource"}};
}

std::vector<std::string> GraphCanvasModel::placePasses(std::string_view except) const {
    std::vector<std::string> passes;
    for (const auto &pass : plan_passes_) {
        if (pass.kind == "anchor" || !pass.loop.empty() || pass.name.empty() || pass.name == except)
            continue;
        appendUnique(passes, pass.name);
    }
    return passes;
}

std::vector<std::string> GraphCanvasModel::passFamilies() const {
    std::vector<std::string> families;
    const auto graphs = render_authoring_.find("graphs");
    if (graphs == render_authoring_.end() || !graphs->is_array()) return families;
    for (const auto &graph : *graphs)
        if (const auto name = text(graph, "name"); !name.empty()) appendUnique(families, name);
    return families;
}

bool GraphCanvasModel::showsPlace(std::string_view op) noexcept {
    return std::ranges::find(kPlacedRenderOps, op) != kPlacedRenderOps.end();
}

bool GraphCanvasModel::takesPlace(std::string_view op) noexcept {
    return showsPlace(op) && op != "render.compute";
}

GraphCanvasPlace GraphCanvasModel::place(std::string_view scope, std::string_view node) const {
    GraphCanvasPlace result;
    const auto parameters = nodeParameters(scope, node);
    if (!parameters.is_object()) return result;
    result.pass_set = text(parameters, "pass_set");
    if (const auto insert = parameters.find("insert"); insert != parameters.end()) {
        result.kind = "other";
        result.target = insert->dump();
        if (!insert->is_object()) return result;
        const auto declared = insert->find("declared");
        result.declared = declared != insert->end() && declared->is_boolean() && declared->get<bool>();
        std::size_t sides = 0;
        for (const auto &[key, value] : insert->items()) {
            if (key == "declared") continue;
            ++sides;
            if ((key == "before" || key == "after") && value.is_string() &&
                !value.get<std::string>().empty()) {
                result.kind = key;
                result.target = value.get<std::string>();
            }
        }
        if (sides != 1) {
            result.kind = "other";
            result.target = insert->dump();
        }
        return result;
    }
    if (const auto anchor = text(parameters, "canonical_anchor"); !anchor.empty()) {
        result.kind = "anchor";
        result.target = anchor;
    }
    return result;
}

bool GraphCanvasModel::setPlace(std::string_view scope, const std::string &node,
                                const GraphCanvasPlace &place) {
    const auto *found = findNode(scope, node);
    if (found == nullptr || !takesPlace(text(*found, "op"))) return false;
    if (place.kind == "other") return false;
    if ((place.kind == "before" || place.kind == "after" || place.kind == "anchor") &&
        place.target.empty())
        return false;
    if (place.kind == "anchor" &&
        std::ranges::find(loopAnchors(), place.target) == loopAnchors().end())
        return false;
    auto parameters = nodeParameters(scope, node);
    // One place at a time: an insert and an anchor exclude each other.
    parameters.erase("insert");
    parameters.erase("canonical_anchor");
    if (place.kind == "before" || place.kind == "after") {
        Json insert{{place.kind, place.target}};
        // A material pass among the declared passes stays among them; the
        // pass kinds' insert takes no `declared` (graphmodule).
        if (place.declared && text(*found, "op") == kMaterialPass) insert["declared"] = true;
        parameters["insert"] = std::move(insert);
    } else if (place.kind == "anchor") {
        parameters["canonical_anchor"] = place.target;
    }
    if (place.pass_set.empty()) parameters.erase("pass_set");
    else parameters["pass_set"] = place.pass_set;
    return setNodeParameters(scope, node, parameters);
}

std::optional<GraphCanvasPortSettings>
GraphCanvasModel::portSettings(std::string_view scope, std::string_view node,
                               std::string_view port) const {
    const auto *found = findNode(scope, node);
    if (found == nullptr || port.empty()) return std::nullopt;
    const auto op = text(*found, "op");
    if (op != "render.fullscreen" && op != "render.raster" && op != "render.compute")
        return std::nullopt;
    const auto parameters = nodeParameters(scope, node);
    const auto ports = parameters.value("ports", Json::object());
    if (!ports.is_object()) return std::nullopt;
    const auto entry = ports.find(std::string{port});
    if (entry == ports.end() || !entry->is_object()) return std::nullopt;
    const auto &declaration = *entry;
    GraphCanvasPortSettings settings;
    const bool image = declaration.value("kind", std::string{"buffer"}) == "image";
    const auto access = text(declaration, "access");
    const auto attachment = text(declaration, "attachment");
    const auto binding = text(declaration, "binding");
    const bool graphics = op != "render.compute";
    // graphmodule.cpp / rendergraphcomposition.cpp, restated: a previous
    // frame is read, never written; an attachment takes no shader resource
    // setting. UX-7, decision 1 (engine RS-14): a numbered input takes
    // sampling and a subresource as a named port does (input_settings); its
    // view stays the shader's.
    settings.history_allowed = image && access == "read" && attachment.empty();
    settings.sampling_allowed = image && access == "read" && attachment.empty() &&
                                (graphics ? binding != "storage" : binding == "sampled");
    settings.subresource_allowed = image && attachment.empty();
    settings.location_allowed = graphics && attachment == "color";
    if (const auto history = declaration.find("history");
        history != declaration.end() && history->is_boolean())
        settings.history = history->get<bool>();
    if (const auto sampling = declaration.find("sampling");
        sampling != declaration.end() && sampling->is_object()) {
        settings.filter = text(*sampling, "filter");
        settings.address = text(*sampling, "address");
    }
    if (const auto subresource = declaration.find("subresource");
        subresource != declaration.end() && subresource->is_object()) {
        settings.mip = subresource->value("mip", std::int64_t{0});
        settings.layer = subresource->value("layer", std::int64_t{0});
    }
    if (const auto location = declaration.find("location");
        location != declaration.end() && location->is_number_integer())
        settings.location = location->get<std::int64_t>();
    return settings;
}

bool GraphCanvasModel::setPortSettings(std::string_view scope, const std::string &node,
                                       const std::string &port,
                                       const GraphCanvasPortSettings &settings) {
    const auto current = portSettings(scope, node, port);
    if (!current) return false;
    auto parameters = nodeParameters(scope, node);
    auto &declaration = parameters["ports"][port];
    if (current->history_allowed) {
        if (settings.history) declaration["history"] = true;
        else declaration.erase("history");
    }
    if (current->sampling_allowed) {
        Json sampling = Json::object();
        if (!settings.filter.empty()) sampling["filter"] = settings.filter;
        if (!settings.address.empty()) sampling["address"] = settings.address;
        if (sampling.empty()) declaration.erase("sampling");
        else declaration["sampling"] = std::move(sampling);
    }
    if (current->subresource_allowed) {
        auto subresource = declaration.value("subresource", Json::object());
        if (!subresource.is_object()) subresource = Json::object();
        // Mip and layer 0 are what a port without the key reads; the
        // counts the document holds stay.
        if (settings.mip > 0) subresource["mip"] = settings.mip;
        else subresource.erase("mip");
        if (settings.layer > 0) subresource["layer"] = settings.layer;
        else subresource.erase("layer");
        if (subresource.empty()) declaration.erase("subresource");
        else declaration["subresource"] = std::move(subresource);
    }
    if (current->location_allowed && settings.location >= 0)
        declaration["location"] = settings.location;
    return setNodeParameters(scope, node, parameters);
}

namespace {

// UX-7, decision 3: the port of a raster's parameters that is its depth
// attachment; empty for none.
std::string rasterDepthPort(const Json &parameters) {
    const auto ports = parameters.is_object() ? parameters.value("ports", Json::object()) : Json::object();
    if (!ports.is_object()) return {};
    for (const auto &[port, declaration] : ports.items())
        if (declaration.is_object() && text(declaration, "attachment") == "depth") return port;
    return {};
}

const Json &depthImageType() {
    static const Json type{{"semantic", "pelican.render.depth@1"},
                           {"arguments", Json{{"representation", "device"}, {"space", "projection"}}}};
    return type;
}

bool depthImageNode(const Json &node) {
    if (!node.is_object() || text(node, "op") != "render.image") return false;
    const auto parameters = node.value("parameters", Json::object());
    if (!parameters.is_object()) return false;
    const auto format = text(parameters, "format");
    if (format.starts_with("D16") || format.starts_with("D24") || format.starts_with("D32") ||
        format.starts_with("X8_D24"))
        return true;
    const auto type = parameters.value("type", Json::object());
    return type.is_object() && text(type, "semantic") == "pelican.render.depth@1";
}

} // namespace

std::string GraphCanvasModel::rasterDepth(std::string_view scope, std::string_view node) const {
    const auto *found = findNode(scope, node);
    if (found == nullptr || text(*found, "op") != "render.raster") return {};
    const auto port = rasterDepthPort(nodeParameters(scope, node));
    if (port.empty()) return {};
    const auto inputs = found->value("inputs", Json::object());
    if (!inputs.is_object()) return {};
    const auto edge = inputs.find(port);
    return edge != inputs.end() && edge->is_object() ? text(*edge, "node") : std::string{};
}

std::vector<std::string> GraphCanvasModel::rasterDepthChoices(std::string_view scope) const {
    std::vector<std::string> result;
    if (const auto *nodes = scopeNodes(scope))
        for (const auto &node : *nodes)
            if (depthImageNode(node)) result.push_back(text(node, "id"));
    return result;
}

bool GraphCanvasModel::setRasterDepth(std::string_view scope, const std::string &node,
                                      const std::string &depth) {
    if (!canEdit()) return false;
    const auto *found = findNode(scope, node);
    if (found == nullptr || text(*found, "op") != "render.raster") return false;
    if (depth == rasterDepth(scope, node) && !depth.empty()) return false;
    Json replaced = *found;
    auto parameters = nodeParameters(scope, node);
    if (!parameters.is_object()) parameters = Json::object();
    if (!parameters.contains("ports") || !parameters.at("ports").is_object())
        parameters["ports"] = Json::object();
    auto inputs = replaced.value("inputs", Json::object());
    if (!inputs.is_object()) inputs = Json::object();
    auto outputs = replaced.value("outputs", Json::array());
    if (!outputs.is_array()) outputs = Json::array();
    const auto token = scopeToken(scope);
    Json ops = Json::array();
    std::string image = depth;
    Json type = depthImageType();
    if (depth == kNewRasterDepth) {
        // Beside the pass, as an Add fullscreen pass's colour image is, and
        // the size of what the pass draws into: a pass's attachments are
        // one size (the colour image's extent_scale, width, height, layers).
        image = node + "__depth";
        for (int suffix = 2; findNode(scope, image) != nullptr && suffix < 10000; ++suffix)
            image = node + "__depth_" + std::to_string(suffix);
        Json image_parameters{{"type", type}, {"format", "D32_SFLOAT"}, {"extent_scale", 1.0},
                              {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})}};
        for (const auto &[port, declaration] : parameters.at("ports").items()) {
            if (!declaration.is_object() || text(declaration, "attachment") != "color") continue;
            const auto edge = inputs.find(port);
            if (edge == inputs.end() || !edge->is_object()) continue;
            const auto *color = findNode(scope, text(*edge, "node"));
            if (color == nullptr || text(*color, "op") != "render.image") continue;
            const auto shape = color->value("parameters", Json::object());
            for (const char *key : {"extent_scale", "width", "height", "layers"})
                if (shape.is_object() && shape.contains(key)) image_parameters[key] = shape.at(key);
            break;
        }
        ops.push_back(Json{{"op", "add_node"}, {"scope", token},
                           {"node", Json{{"id", image}, {"op", "render.image"},
                                         {"parameters", std::move(image_parameters)},
                                         {"outputs", Json::array({"resource"})}}}});
    } else if (!depth.empty()) {
        const auto choices = rasterDepthChoices(scope);
        if (std::ranges::find(choices, depth) == choices.end()) return false;
        const auto *picked = findNode(scope, depth);
        const auto declared = picked->value("parameters", Json::object()).value("type", Json{});
        if (declared.is_object()) type = declared;
    }
    auto port = rasterDepthPort(parameters);
    if (!port.empty()) {
        parameters["ports"].erase(port);
        inputs.erase(port);
        outputs.erase(std::remove(outputs.begin(), outputs.end(), Json(port)), outputs.end());
    }
    auto state = parameters.value("raster_state", Json::object());
    if (!state.is_object()) state = Json::object();
    if (image.empty()) {
        for (const char *key : {"depth_test", "depth_write", "depth_compare"}) state.erase(key);
    } else {
        if (port.empty()) {
            port = "depth";
            for (int suffix = 2; parameters.at("ports").contains(port) && suffix < 10000; ++suffix)
                port = "depth_" + std::to_string(suffix);
        }
        parameters["ports"][port] = Json{{"kind", "image"}, {"type", type}, {"access", "write"},
                                         {"attachment", "depth"}};
        inputs[port] = Json{{"node", image}, {"port", "resource"}};
        outputs.push_back(port);
        // Nearer surfaces in front: what a depth attachment is given for.
        if (!state.contains("depth_test")) state["depth_test"] = true;
        if (!state.contains("depth_write")) state["depth_write"] = true;
    }
    if (state.empty()) parameters.erase("raster_state");
    else parameters["raster_state"] = std::move(state);
    replaced["parameters"] = std::move(parameters);
    replaced["inputs"] = std::move(inputs);
    replaced["outputs"] = std::move(outputs);
    if (replaced == *found) return false;
    ops.push_back(Json{{"op", "replace_node"}, {"scope", token}, {"node", std::move(replaced)}});
    return submitOps(std::move(ops));
}

std::vector<std::string> GraphCanvasModel::valueInputNames() {
    return {"scene_time", "time", "delta_time"};
}

std::vector<std::string> GraphCanvasModel::bufferChoices(std::string_view scope) const {
    std::vector<std::string> result;
    for (const std::string_view each : {std::string_view{}, scope}) {
        const auto *nodes = scopeNodes(each);
        if (nodes == nullptr) continue;
        for (const auto &node : *nodes)
            if (node.is_object() && text(node, "op") == "render.buffer") {
                const auto id = text(node, "id");
                if (!id.empty() && std::ranges::find(result, id) == result.end()) result.push_back(id);
            }
        if (scope.empty()) break;
    }
    return result;
}

std::string GraphCanvasModel::proposeDefinitionName(std::string_view definition) const {
    const auto referrers = definitionReferrers(definition);
    if (referrers.empty()) return std::string{definition};
    const auto base = referrers.front() + "_body";
    if (base == definition) return std::string{definition};
    return suggestDefinitionName(base);
}

std::vector<GraphCanvasMaterial> GraphCanvasModel::materials() const {
    std::vector<GraphCanvasMaterial> result;
    const auto found = render_authoring_.find("materials");
    if (found == render_authoring_.end() || !found->is_array()) return result;
    for (const auto &entry : *found) {
        if (!entry.is_object()) continue;
        GraphCanvasMaterial material;
        material.id = text(entry, "id");
        if (material.id.empty()) continue;
        material.name = text(entry, "name");
        if (material.name.empty()) material.name = material.id;
        material.tags = stringsAt(entry, "tags");
        material.variants = stringsAt(entry, "variants");
        material.builtin = entry.value("builtin", false);
        result.push_back(std::move(material));
    }
    return result;
}

std::vector<std::string> GraphCanvasModel::materialTags() const {
    return stringsAt(render_authoring_, "material_tags");
}

std::vector<std::string> GraphCanvasModel::materialVariants() const {
    std::set<std::string> variants;
    for (const auto &material : materials())
        variants.insert(material.variants.begin(), material.variants.end());
    return {variants.begin(), variants.end()};
}

bool GraphCanvasModel::setMaterialPassVariant(const std::string &node, const std::string &variant) {
    const auto *found = findNode({}, node);
    if (found == nullptr || text(*found, "op") != kMaterialPass) return false;
    auto parameters = nodeParameters({}, node);
    if (variant.empty()) parameters.erase("material_variant");
    else parameters["material_variant"] = variant;
    return setNodeParameters({}, node, parameters);
}

// --- UX-5b: the Graph's fullscreen pass used as the Pass Editor's is.

std::vector<GraphCanvasEngineShader> GraphCanvasModel::engineShaders() const {
    std::vector<GraphCanvasEngineShader> result;
    for (const auto &entry : engine_shaders_) {
        if (!entry.is_object()) continue;
        GraphCanvasEngineShader shader;
        shader.reference = text(entry, "reference");
        shader.stage = text(entry, "stage");
        if (shader.reference.empty() || shader.stage.empty()) continue;
        // EL-4: a source that includes the generated resource-port header
        // neither compiles nor reflects alone; the feature's JSON gives it
        // its ports.
        shader.ports_by_hand =
            entry.contains("reflection_error") && entry.value("resource_port_include", false);
        for (const auto &port : entry.value("resource_ports", Json::array())) {
            if (!port.is_object()) continue;
            const auto name = text(port, "port");
            if (name.empty()) continue;
            const auto type = text(port, "type");
            shader.resource_ports.push_back(type.empty() ? name : name + " (" + type + ")");
        }
        result.push_back(std::move(shader));
    }
    return result;
}

void GraphCanvasModel::askEngineShaders() {
    if (engine_shaders_known_ || engine_shaders_asked_) return;
    // The catalog says first whether it carries the list (an EL-4 engine).
    if (!connected_ || !catalog_received_) {
        engine_shaders_wanted_ = true;
        return;
    }
    engine_shaders_wanted_ = false;
    engine_shaders_asked_ = true;
    read(RequestKind::EngineShaders, "get_engine_shaders", Json::object());
}

void GraphCanvasModel::takeEngineShaders(const Json &shaders) {
    engine_shaders_ = shaders.is_array() ? shaders : Json::array();
    engine_shaders_known_ = true;
    engine_shaders_wanted_ = false;
    ++engine_shaders_revision_;
    // Each shader's pass_inputs are read as a plan's are, so a pass drawing
    // with it has its sockets before the plan that runs it arrives.
    engine_shader_inputs_.clear();
    for (const auto &shader : engine_shaders_) {
        if (!shader.is_object() || !shader.contains("pass_inputs")) continue;
        const auto reference = text(shader, "reference");
        if (reference.empty() || engine_shader_inputs_.contains(reference)) continue;
        if (text(shader, "stage") != "fragment") continue;
        const Json node{{"pass_inputs",
                         Json{{"state", "declared"},
                              {"fragment", reference},
                              {"declarations", shader.at("pass_inputs")},
                              {"variadic_range", shader.value("variadic_inputs", Json(nullptr))}}}};
        try {
            if (auto inputs = framePlanPassInputs(node, "engine_shaders"))
                engine_shader_inputs_.emplace(reference, std::move(*inputs));
        } catch (const std::exception &) {
            // A shader whose declarations Studio cannot read keeps the plan's.
        }
    }
}

std::vector<GraphCanvasRayTracingPipeline> GraphCanvasModel::rayTracingPipelines() const {
    std::vector<GraphCanvasRayTracingPipeline> pipelines;
    // The stems with a raygen stage, in the list's order.
    for (const auto &entry : engine_shaders_)
        if (entry.is_object() && text(entry, "stage") == "raygen" && !text(entry, "reference").empty() &&
            std::ranges::find(pipelines, text(entry, "reference"),
                              &GraphCanvasRayTracingPipeline::reference) == pipelines.end())
            pipelines.push_back({text(entry, "reference"), text(entry, "reference"), {}, {}, {}, {}});
    for (auto &pipeline : pipelines)
        for (const auto &entry : engine_shaders_) {
            if (!entry.is_object() || text(entry, "reference") != pipeline.reference) continue;
            const auto stage = text(entry, "stage");
            if (stage == "miss") pipeline.miss = pipeline.reference;
            if (stage == "closesthit") pipeline.closesthit = pipeline.reference;
            for (const auto &port : entry.value("resource_ports", Json::array())) {
                const auto name = port.is_object() ? text(port, "port") : std::string{};
                if (name.empty() ||
                    std::ranges::any_of(pipeline.ports, [&](const auto &known) { return known.first == name; }))
                    continue;
                pipeline.ports.emplace_back(name, text(port, "type"));
                if (const auto format = text(port, "format"); !format.empty())
                    pipeline.formats[name] = format;
            }
        }
    // A pipeline traces with all three.
    std::erase_if(pipelines, [](const GraphCanvasRayTracingPipeline &pipeline) {
        return pipeline.miss.empty() || pipeline.closesthit.empty();
    });
    return pipelines;
}

std::optional<GraphCanvasEngineShader>
GraphCanvasModel::engineShader(std::string_view reference) const {
    for (auto &shader : engineShaders())
        if (shader.reference == reference) return std::move(shader);
    return std::nullopt;
}

std::string GraphCanvasModel::formatAspect(std::string_view format) const {
    const auto vocabulary = render_authoring_.find("render_target_vocabulary");
    if (vocabulary == render_authoring_.end() || !vocabulary->is_object()) return {};
    const auto aspects = vocabulary->find("format_aspects");
    if (aspects == vocabulary->end() || !aspects->is_object()) return {};
    const auto found = aspects->find(std::string{format});
    return found != aspects->end() && found->is_string() ? found->get<std::string>()
                                                         : std::string{};
}

bool GraphCanvasModel::isDepthFormat(std::string_view format) const {
    const auto aspect = formatAspect(format);
    if (!aspect.empty()) return aspect != "color";
    return depthFormat(format);
}

std::optional<GraphCanvasMaterialPassImage>
GraphCanvasModel::materialPassImage(std::string_view scope, std::string_view image) const {
    // A material pass stands in the root only (addNodeOps).
    if (!scope.empty() || material_pass_contracts_.empty()) return std::nullopt;
    for (const auto &pass : nodes_) {
        if (!pass.is_object() || text(pass, "op") != kMaterialPass) continue;
        const auto parameters = pass.value("parameters", Json::object());
        // Outputs of the author's own: the contract's table is not theirs.
        if (!parameters.is_object() || parameters.contains("material_outputs")) continue;
        const auto inputs = pass.value("inputs", Json::object());
        if (!inputs.is_object()) continue;
        for (const auto &[socket, edge] : inputs.items()) {
            if (!edge.is_object() || text(edge, "node") != image) continue;
            const auto contract = text(parameters, "material_contract");
            for (const auto &entry : material_pass_contracts_) {
                if (!entry.is_object() || text(entry, "contract") != contract) continue;
                for (const auto &declared : entry.value("sockets", Json::array())) {
                    if (!declared.is_object() || text(declared, "socket") != socket) continue;
                    GraphCanvasMaterialPassImage result;
                    result.pass = text(pass, "id");
                    result.contract = contract;
                    result.socket = socket;
                    for (const auto &format : declared.value("formats", Json::array()))
                        if (format.is_string()) result.formats.push_back(format.get<std::string>());
                    return result;
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> GraphCanvasModel::imageFormatChoicesFor(std::string_view scope,
                                                                 std::string_view node) const {
    if (const auto socket = materialPassImage(scope, node); socket && !socket->formats.empty())
        return socket->formats;
    return imageFormatChoices(nodeParameters(scope, node));
}

bool GraphCanvasModel::setImageFormat(std::string_view scope, const std::string &node,
                                      const std::string &format) {
    if (!canEdit() || format.empty() || findNode(scope, node) == nullptr) return false;
    const auto token = scopeToken(scope);
    Json ops = Json::array();
    const auto put = [&](const std::string &image, const std::string &value) {
        auto parameters = nodeParameters(scope, image);
        if (!parameters.is_object() || parameters.value("format", std::string{}) == value) return;
        parameters["format"] = value;
        ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", image},
                           {"parameters", std::move(parameters)}});
    };
    put(node, format);
    if (const auto socket = materialPassImage(scope, node)) {
        // The combination the new format belongs to: of those that hold it
        // at this socket, the one the other images already match most.
        const auto *pass = findNode({}, socket->pass);
        const auto inputs = pass != nullptr ? pass->value("inputs", Json::object()) : Json::object();
        const auto imageAt = [&](const std::string &other) {
            const auto edge = inputs.is_object() ? inputs.value(other, Json{}) : Json{};
            return edge.is_object() ? text(edge, "node") : std::string{};
        };
        std::optional<Json> chosen;
        int chosen_matches = -1;
        for (const auto &entry : material_pass_contracts_) {
            if (!entry.is_object() || text(entry, "contract") != socket->contract) continue;
            for (const auto &combination : entry.value("combinations", Json::array())) {
                if (!combination.is_object() || text(combination, socket->socket.c_str()) != format)
                    continue;
                int matches = 0;
                for (const auto &[other, value] : combination.items()) {
                    const auto image = imageAt(other);
                    if (image.empty() || !value.is_string()) continue;
                    if (nodeParameters({}, image).value("format", std::string{}) ==
                        value.get<std::string>())
                        ++matches;
                }
                if (matches > chosen_matches) {
                    chosen = combination;
                    chosen_matches = matches;
                }
            }
        }
        if (chosen)
            for (const auto &[other, value] : chosen->items()) {
                if (other == socket->socket || !value.is_string()) continue;
                const auto image = imageAt(other);
                const auto *found = image.empty() ? nullptr : findNode({}, image);
                if (found == nullptr || text(*found, "op") != "render.image") continue;
                put(image, value.get<std::string>());
            }
    }
    if (ops.empty()) return false;
    return submitOps(std::move(ops));
}

// --- PS-1s (engine PS-1): passes that run once, images that are kept.

namespace {

bool schedulableOp(std::string_view op) {
    return op == "render.fullscreen" || op == "render.raster" || op == "render.compute";
}

// A port the pass writes through, as the engine judges it
// (rendergraphcomposition's once check).
bool writesThrough(const Json &declaration) {
    if (!declaration.is_object()) return false;
    const auto access = text(declaration, "access");
    return access == "write" || access == "read_write" || declaration.contains("attachment");
}

std::string keptNote(const std::vector<std::string> &kept) {
    if (kept.empty()) return {};
    std::string names;
    for (const auto &name : kept) names += (names.empty() ? "" : ", ") + name;
    // One sentence: the notice line is the first one (oneLine).
    return "Saved; " + names + " made Persistent: what a once pass writes is kept.";
}

} // namespace

std::string GraphCanvasModel::nodeSchedule(std::string_view scope, std::string_view node) const {
    const auto *found = findNode(scope, node);
    if (found == nullptr || !schedulableOp(text(*found, "op"))) return {};
    const auto parameters = found->value("parameters", Json::object());
    const auto schedule = parameters.is_object() ? text(parameters, "schedule") : std::string{};
    return schedule.empty() ? std::string{"per_frame"} : schedule;
}

std::vector<std::string> GraphCanvasModel::transientImagesWrittenBy(std::string_view scope,
                                                                    std::string_view node) const {
    std::vector<std::string> images;
    const auto *pass = findNode(scope, node);
    if (pass == nullptr || !schedulableOp(text(*pass, "op"))) return images;
    const auto parameters = pass->value("parameters", Json::object());
    const auto ports = parameters.is_object() ? parameters.value("ports", Json::object())
                                              : Json::object();
    const auto inputs = pass->value("inputs", Json::object());
    if (!ports.is_object() || !inputs.is_object()) return images;
    for (const auto &[port, declaration] : ports.items()) {
        if (!writesThrough(declaration) || !inputs.contains(port)) continue;
        // The image at the end of the chain of passes that hand it down.
        auto endpoint = inputs.at(port);
        std::set<std::string> seen;
        while (endpoint.is_object() && !text(endpoint, "node").empty()) {
            const auto id = text(endpoint, "node");
            const auto *source = findNode(scope, id);
            if (source == nullptr || !seen.insert(id).second) break;
            if (text(*source, "op") == "render.image") {
                const auto image = source->value("parameters", Json::object());
                const bool kept = image.is_object() && text(image, "lifetime") == "persistent";
                if (!kept && std::ranges::find(images, id) == images.end()) images.push_back(id);
                break;
            }
            const auto upstream = source->value("inputs", Json::object());
            const auto through = text(endpoint, "port");
            if (!upstream.is_object() || !upstream.contains(through)) break;
            endpoint = upstream.at(through);
        }
    }
    return images;
}

bool GraphCanvasModel::setNodeSchedule(std::string_view scope, const std::string &node,
                                       const std::string &schedule) {
    if (!canEdit()) return false;
    const auto current = nodeSchedule(scope, node);
    if (current.empty() || current == schedule) return false;
    const bool compute = text(*findNode(scope, node), "op") == "render.compute";
    if (schedule != "per_frame" && schedule != "once" && !(compute && schedule == "per_view"))
        return false;
    const auto token = scopeToken(scope);
    Json ops = Json::array();
    auto parameters = nodeParameters(scope, node);
    if (!parameters.is_object()) return false;
    if (schedule == "per_frame") parameters.erase("schedule");
    else parameters["schedule"] = schedule;
    ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", node},
                       {"parameters", std::move(parameters)}});
    // Decision 2: what a once pass writes has to keep its contents.
    std::vector<std::string> kept;
    if (schedule == "once")
        for (const auto &image : transientImagesWrittenBy(scope, node)) {
            auto image_parameters = nodeParameters(scope, image);
            if (!image_parameters.is_object()) continue;
            image_parameters["lifetime"] = "persistent";
            ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", image},
                               {"parameters", std::move(image_parameters)}});
            kept.push_back(image);
        }
    if (!submitOps(std::move(ops))) return false;
    saved_note_ = keptNote(kept);
    return true;
}

std::optional<bool> GraphCanvasModel::loopBodyOnce(std::string_view loop) const {
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return std::nullopt;
    std::size_t passes = 0;
    std::size_t once = 0;
    std::set<std::string> seen;
    for (const auto &body : loopBranchBodies(*node)) {
        if (body.empty() || !seen.insert(body).second) continue;
        const auto *nodes = scopeNodes(body);
        if (nodes == nullptr) continue;
        for (const auto &candidate : *nodes) {
            if (!candidate.is_object() || !schedulableOp(text(candidate, "op"))) continue;
            ++passes;
            if (nodeSchedule(body, text(candidate, "id")) == "once") ++once;
        }
    }
    if (passes == 0) return std::nullopt;
    return once == passes;
}

bool GraphCanvasModel::setLoopBodyOnce(const std::string &loop, bool once) {
    if (!canEdit()) return false;
    const auto *node = findNode({}, loop);
    if (node == nullptr || !isLoop(*node)) return false;
    Json ops = Json::array();
    std::vector<std::string> kept;
    std::set<std::string> seen;
    for (const auto &body : loopBranchBodies(*node)) {
        if (body.empty() || !seen.insert(body).second) continue;
        const auto *nodes = scopeNodes(body);
        if (nodes == nullptr) continue;
        const auto token = scopeToken(body);
        for (const auto &candidate : *nodes) {
            if (!candidate.is_object() || !schedulableOp(text(candidate, "op"))) continue;
            const auto id = text(candidate, "id");
            const auto current = nodeSchedule(body, id);
            // Off: a once pass back to every frame; a task that ran per view
            // keeps that.
            if (once ? current == "once" : current != "once") continue;
            auto parameters = nodeParameters(body, id);
            if (!parameters.is_object()) continue;
            if (once) parameters["schedule"] = "once";
            else parameters.erase("schedule");
            ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", id},
                               {"parameters", std::move(parameters)}});
            if (!once) continue;
            for (const auto &image : transientImagesWrittenBy(body, id)) {
                if (std::ranges::find(kept, image) != kept.end()) continue;
                auto image_parameters = nodeParameters(body, image);
                if (!image_parameters.is_object()) continue;
                image_parameters["lifetime"] = "persistent";
                ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token},
                                   {"node", image}, {"parameters", std::move(image_parameters)}});
                kept.push_back(image);
            }
        }
    }
    if (ops.empty() || !submitOps(std::move(ops))) return false;
    saved_note_ = keptNote(kept);
    return true;
}

std::vector<const GraphCanvasPlanPass *>
GraphCanvasModel::planPassesMadeBy(std::string_view scope, std::string_view node) const {
    std::vector<const GraphCanvasPlanPass *> passes;
    if (node.empty()) return passes;
    if (scope.empty()) {
        for (const auto &name : planNodesOf(node))
            if (const auto *pass = planPass(name)) passes.push_back(pass);
        return passes;
    }
    for (const auto &pass : plan_passes_) {
        const bool made = pass.origin
                              ? pass.origin->definition == scope && pass.origin->node == node
                              : !pass.loop.empty() && pass.loop_node == node;
        if (made) passes.push_back(&pass);
    }
    return passes;
}

std::optional<GraphCanvasOnceRun> GraphCanvasModel::onceRun(std::string_view scope,
                                                            std::string_view node) const {
    if (nodeSchedule(scope, node) != "once") return std::nullopt;
    GraphCanvasOnceRun run;
    for (const auto *pass : planPassesMadeBy(scope, node)) {
        if (!pass->once) continue;
        ++run.passes;
        if (pass->last_run_generation)
            run.generation = std::max(run.generation.value_or(0), *pass->last_run_generation);
        if (pass->last_run_frame)
            run.frame = std::max(run.frame.value_or(0), *pass->last_run_frame);
    }
    return run;
}

std::string GraphCanvasModel::onceBand(std::string_view scope, std::string_view node) const {
    const auto run = onceRun(scope, node);
    if (!run) return {};
    return "once" + std::string{kDot} +
           (run->generation ? "ran gen " + std::to_string(*run->generation)
                            : std::string{"not run yet"});
}

bool GraphCanvasModel::persistentImage(std::string_view scope, std::string_view node) const {
    const auto *found = findNode(scope, node);
    if (found == nullptr || text(*found, "op") != "render.image") return false;
    const auto parameters = found->value("parameters", Json::object());
    return parameters.is_object() && text(parameters, "lifetime") == "persistent";
}

std::string GraphCanvasModel::imageInitialAsset(std::string_view scope, std::string_view node) const {
    const auto *found = findNode(scope, node);
    if (found == nullptr || text(*found, "op") != "render.image") return {};
    const auto parameters = found->value("parameters", Json::object());
    if (!parameters.is_object()) return {};
    const auto initial = parameters.find("initial");
    return initial != parameters.end() && initial->is_object() ? text(*initial, "asset")
                                                               : std::string{};
}

std::vector<std::string> GraphCanvasModel::initialAssets() const {
    std::set<std::string> assets;
    for (const auto &scope : scopes())
        for (const auto &node : nodes(scope.id)) {
            if (node.op == "render.image")
                if (auto asset = imageInitialAsset(scope.id, node.id); !asset.empty())
                    assets.insert(std::move(asset));
            // UX-13a, decision 3: a value.sample_table's table too, which the
            // engine reads again in a new generation when it changed (IA-3)
            // -- a Re-bake is picked up as any rewrite on disk is. Named as an
            // image's initial asset is, so one file is watched once.
            if (node.op == "value.sample_table")
                if (auto asset = sampleTableAssetOf(scope.id, node.id); !asset.empty())
                    assets.insert(std::move(asset));
        }
    return {assets.begin(), assets.end()};
}

bool GraphCanvasModel::rerunOnce(std::string_view scope, const std::string &node) {
    if (!connected_) return false;
    // A body node is no node of the plan; its loop is, and runs its copies.
    auto target = node;
    if (!scope.empty() && !node.empty()) {
        target.clear();
        for (const auto &candidate : nodes_)
            if (isLoop(candidate) && runsBody(candidate, scope)) {
                target = text(candidate, "id");
                break;
            }
        if (target.empty()) return false;
    }
    once_rerun_node_ = target;
    read(RequestKind::RerunOnce, "rerun_once",
         target.empty() ? Json::object() : Json{{"node", target}});
    return true;
}

bool GraphCanvasModel::planHasOncePasses() const {
    return std::ranges::any_of(plan_passes_, &GraphCanvasPlanPass::once);
}

void GraphCanvasModel::receiveRerunOnce(const Json &response) {
    if (!response.is_object()) {
        receiveRerunOnceRefusal("the engine's answer could not be read");
        return;
    }
    if (const auto error = response.find("error"); error != response.end()) {
        auto message = error->is_string()   ? error->get<std::string>()
                       : error->is_object() ? text(*error, "message")
                                            : std::string{};
        if (error->is_object())
            if (const auto data = error->find("data");
                data != error->end() && data->is_object() && text(*data, "code") == "node_unknown")
                message = "node_unknown";
        receiveRerunOnceRefusal(message);
        return;
    }
    std::vector<std::string> scheduled;
    for (const auto &name : response.value("scheduled", Json::array()))
        if (name.is_string()) scheduled.push_back(name.get<std::string>());
    const auto node = std::exchange(once_rerun_node_, {});
    if (scheduled.empty()) {
        edit_notice_ = {InspectorNoticeKind::Information,
                        node.empty() ? std::string{"No pass runs once: there is nothing to recompute."}
                                     : node + " runs every frame: there is nothing to recompute."};
    } else {
        edit_notice_ = {InspectorNoticeKind::Information,
                        "Recompute: " + std::to_string(scheduled.size()) +
                            (scheduled.size() == 1 ? " once pass runs" : " once passes run") +
                            " again on the next frame."};
        // The plan is read again until one of them says it ran after now.
        once_follow_from_.clear();
        for (const auto &name : scheduled) {
            const auto *pass = planPass(name);
            once_follow_from_[name] = pass != nullptr ? pass->last_run_frame.value_or(0) : 0;
        }
        once_follow_reads_ = onceFollowLimit;
    }
    ++report_revision_;
}

void GraphCanvasModel::receiveRerunOnceRefusal(const std::string &message) {
    const auto node = std::exchange(once_rerun_node_, {});
    // rerun_once's invalid params carry {code: "node_unknown", node} beside
    // the engine's sentence, which is all of it that reaches Studio.
    std::string line;
    if (message == "node_unknown" || message.find("names no node") != std::string::npos)
        line = graphLoopRefusalDescription(Json{{"code", "node_unknown"}, {"node", node}});
    if (line.empty()) line = "Recompute was refused: " + message;
    edit_notice_ = {InspectorNoticeKind::Error, std::move(line)};
    ++report_revision_;
}

std::vector<GraphCanvasTemplateSource>
GraphCanvasModel::templateSources(std::string_view scope, std::string_view node) const {
    std::vector<GraphCanvasTemplateSource> sources;
    const auto *nodes = scopeNodes(scope);
    if (nodes == nullptr) return sources;
    for (const auto &candidate : *nodes) {
        if (!candidate.is_object()) continue;
        const auto id = text(candidate, "id");
        const auto op = text(candidate, "op");
        if (id.empty()) continue;
        if (op == "render.image") sources.push_back({id, id, "resource"});
        // A target declared outside the graph is read under its own name.
        if (op == "render.target_ref") {
            const auto name = text(candidate.value("parameters", Json::object()), "name");
            if (!name.empty()) sources.push_back({name, id, "resource"});
        }
    }
    std::set<std::string> written;
    // GL-1f: an image a pass writes is read from that pass's output of the
    // socket's name, so the reader waits on the pass.
    for (const auto &writer : *nodes) {
        if (!writer.is_object()) continue;
        const auto id = text(writer, "id");
        if (id.empty() || id == node) continue;
        const auto inputs = writer.value("inputs", Json::object());
        if (!inputs.is_object()) continue;
        // Its listed outputs (a fullscreen pass's attachment port), and the
        // sockets it hands on (a material pass's, GL-1f).
        std::vector<std::string> ports;
        for (const auto &output : writer.value("outputs", Json::array()))
            if (output.is_string()) ports.push_back(output.get<std::string>());
        const auto op = text(writer, "op");
        if (op == kMaterialPass) {
            for (const auto &socket :
                 materialPassColorSockets(writer.value("parameters", Json::object())))
                ports.push_back(socket);
            ports.emplace_back("depth");
        } else if (op == "render.shadow_depth") {
            ports.emplace_back("depth");
        } else if (op == "render.snapshot_copy") {
            ports.emplace_back("destination");
        }
        for (const auto &port : ports) {
            const auto forwarded = forwardedInputOf(writer, port);
            const auto edge = inputs.value(forwarded.empty() ? port : forwarded, Json{});
            if (!edge.is_object()) continue;
            const auto image = text(edge, "node");
            const auto found =
                std::ranges::find(sources, image, &GraphCanvasTemplateSource::resource);
            if (found == sources.end() || found->node != image ||
                !written.insert(image).second)
                continue;
            found->node = id;
            found->port = port;
        }
    }
    // The pass's own image is what it writes, not what it reads.
    if (const auto *self = findNode(scope, node)) {
        const auto inputs = self->value("inputs", Json::object());
        const auto outputs = self->value("outputs", Json::array());
        if (inputs.is_object() && outputs.is_array())
            for (const auto &output : outputs) {
                if (!output.is_string()) continue;
                const auto edge = inputs.value(output.get<std::string>(), Json{});
                if (!edge.is_object()) continue;
                const auto written = text(edge, "node");
                std::erase_if(sources, [&](const GraphCanvasTemplateSource &source) {
                    return source.resource == written;
                });
            }
    }
    return sources;
}

bool GraphCanvasModel::setPassShaderTemplate(std::string_view scope, const std::string &node,
                                             const std::string &fragment, bool uses_light_data,
                                             std::string_view push_constants,
                                             const std::vector<GraphCanvasTemplateSource> &connect) {
    if (!canEdit() || fragment.empty()) return false;
    const auto *target = findNode(scope, node);
    if (target == nullptr || !isGraphicsPass(text(*target, "op"))) return false;
    auto parameters = nodeParameters(scope, node);
    if (!parameters.is_object()) return false;
    if (!parameters.contains("shader") || !parameters.at("shader").is_object())
        parameters["shader"] = Json::object();
    parameters["shader"]["fragment"] = fragment;
    if (uses_light_data) parameters["uses_light_data"] = true;
    if (!push_constants.empty()) parameters["push_constants"] = std::string{push_constants};
    if (!parameters.contains("ports") || !parameters.at("ports").is_object())
        parameters["ports"] = Json::object();
    const auto token = scopeToken(scope);
    const auto numbered = numberedPorts(parameters);
    std::int64_t next = 0;
    while (numbered.contains(next)) ++next;
    Json inputs = Json::array();
    Json added = Json::array();
    for (auto source : connect) {
        if (source.node.empty()) {
            // A declared target the graph does not reference yet (the
            // shadow_directional feature's shadow_map): a reference to it
            // comes in the same edit.
            const auto targets = declaredRenderTargets();
            const auto target = std::ranges::find(targets, source.resource,
                                                  &GraphCanvasDeclaredTarget::name);
            if (target == targets.end()) continue;
            std::string id = source.resource + "_ref";
            for (int suffix = 2; findNode(scope, id) != nullptr && suffix < 10000; ++suffix)
                id = source.resource + "_ref_" + std::to_string(suffix);
            added.push_back(Json{{"op", "add_node"}, {"scope", token},
                                 {"node", Json{{"id", id}, {"op", "render.target_ref"},
                                               {"parameters", Json{{"name", source.resource},
                                                                   {"type", target->type}}},
                                               {"outputs", Json::array({"resource"})}}}});
            source.node = id;
            source.port = "resource";
        }
        const auto base = source.port == "resource" ? source.resource : source.port;
        auto name = suggestInputPortName(scope, node, base);
        const auto stem = name;
        for (int suffix = 2; parameters["ports"].contains(name) && suffix < 10000; ++suffix)
            name = stem + "_" + std::to_string(suffix);
        parameters["ports"][name] = Json{{"kind", "image"},
                                         {"type", findNode(scope, source.node) != nullptr
                                                      ? imageTypeOf(scope, source.node, source.port, 8)
                                                      : added.back().at("node").at("parameters").at("type")},
                                         {"access", "read"},
                                         {"binding", "input"},
                                         {"input_index", next++}};
        clampFullscreenInput(text(*target, "op"), parameters["ports"][name]);
        inputs.push_back(Json{{"op", "set_input"}, {"scope", token}, {"node", node},
                              {"port", name},
                              {"source", Json{{"node", source.node}, {"port", source.port}}}});
    }
    if (parameters == nodeParameters(scope, node)) return false;
    Json ops = std::move(added);
    ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", node},
                       {"parameters", std::move(parameters)}});
    for (auto &op : inputs) ops.push_back(std::move(op));
    return submitOps(std::move(ops));
}

// --- EL-5s: a project's compute / ray tracing shader takes its ports from
// its declarations (get_shader_interface, EL-5).

namespace {

constexpr std::string_view kProjectPrefix = "project://";

// The resource ports Studio wires: images, written or sampled.
std::vector<std::string> imagePortNames(const GraphCanvasShaderInterface &shader) {
    std::vector<std::string> names;
    for (const auto &port : shader.ports)
        if (port.type == "storage_image" || port.type == "sampled_image") names.push_back(port.port);
    return names;
}

} // namespace

void GraphCanvasModel::setProjectRayTracingChoices(std::vector<std::string> choices) {
    project_ray_tracing_choices_ = std::move(choices);
}

bool GraphCanvasModel::shaderInterfaceWaiting(std::string_view reference) const {
    return shader_interface_again_at_.contains(reference) ||
           std::ranges::any_of(shader_interface_reads_,
                               [&](const auto &entry) { return entry.second == reference; });
}

void GraphCanvasModel::askShaderInterface(const std::string &reference, bool refresh) {
    if (reference.empty() || !connected_) return;
    const auto asked = [&](const auto &entry) { return entry.second == reference; };
    // One question per stem in the air; a refresh replaces it, so the answer
    // to the older question is dropped when it arrives.
    if (!refresh && std::ranges::any_of(shader_interface_reads_, asked)) return;
    std::erase_if(shader_interface_reads_, asked);
    shader_interface_again_at_.erase(reference);
    const auto id = next_request_++;
    shader_interface_reads_.emplace(id, reference);
    Json params{{"reference", reference}};
    if (refresh) params["refresh"] = true;
    requests_.push_back({id, "get_shader_interface", std::move(params)});
}

std::optional<GraphCanvasShaderInterface>
GraphCanvasModel::shaderInterface(std::string_view reference) const {
    const auto found = shader_interfaces_.find(reference);
    if (found == shader_interfaces_.end()) return std::nullopt;
    return found->second;
}

bool GraphCanvasModel::takeShaderInterfaceReply(std::uint64_t id, const std::string *json,
                                                const std::string &failure) {
    const auto found = shader_interface_reads_.find(id);
    if (found == shader_interface_reads_.end()) return false;
    const auto reference = found->second;
    shader_interface_reads_.erase(found);
    GraphCanvasShaderInterface answer;
    answer.reference = reference;
    answer.status = "failed";
    answer.error = failure;
    if (json != nullptr) {
        try {
            const auto response = Json::parse(*json);
            const auto status = response.is_object() ? text(response, "status") : std::string{};
            if (status == "building") {
                // Asked again half a second later, from poll(). What the
                // canvas had for the stem stays until the new answer.
                shader_interface_again_at_[reference] =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
                if (!shader_interfaces_.contains(reference)) {
                    answer.status = "building";
                    answer.error.clear();
                    shader_interfaces_[reference] = std::move(answer);
                    ++shader_interface_revision_;
                }
                return true;
            }
            if (status == "ready") {
                answer.status = "ready";
                answer.error.clear();
                bool raygen = false;
                bool miss = false;
                bool closesthit = false;
                for (const auto &stage : response.value("stages", Json::array())) {
                    if (!stage.is_object()) continue;
                    const auto name = text(stage, "stage");
                    raygen = raygen || name == "raygen";
                    miss = miss || name == "miss";
                    closesthit = closesthit || name == "closesthit";
                    if (name == "compute") {
                        answer.compute = true;
                        const auto size = stage.value("local_size", Json::array());
                        if (size.is_array() && size.size() == 3)
                            for (std::size_t axis = 0; axis < 3; ++axis)
                                if (size[axis].is_number_integer())
                                    answer.local_size[axis] = size[axis].get<std::int64_t>();
                    }
                    if (answer.reflection_error.empty() && stage.contains("reflection_error"))
                        answer.reflection_error = text(stage, "reflection_error");
                    for (const auto &port : stage.value("resource_ports", Json::array())) {
                        if (!port.is_object() || text(port, "port").empty()) continue;
                        if (std::ranges::any_of(answer.ports, [&](const GraphCanvasShaderPort &known) {
                                return known.port == text(port, "port");
                            }))
                            continue;
                        answer.ports.push_back({text(port, "port"), text(port, "type"), text(port, "format")});
                    }
                }
                answer.ray_tracing = raygen && miss && closesthit;
            } else if (status == "failed") {
                answer.error = text(response, "error");
            } else if (response.is_object() && response.contains("error")) {
                answer.error = diagnostic(response);
            }
        } catch (const std::exception &error) {
            answer.error = error.what();
        }
    }
    shader_interfaces_[reference] = std::move(answer);
    ++shader_interface_revision_;
    // The canvas draws a compute node's empty sockets from it.
    ++report_revision_;
    if (shader_interface_refreshing_.contains(reference)) comparePortsAfterRefresh(reference);
    return true;
}

std::string GraphCanvasModel::computeShaderStem(const Json &parameters) {
    if (!parameters.is_object()) return {};
    if (const auto shader = text(parameters, "shader"); shader.starts_with(kProjectPrefix)) return shader;
    const auto tracing = parameters.find("ray_tracing");
    if (tracing != parameters.end() && tracing->is_object())
        if (const auto raygen = text(*tracing, "raygen"); raygen.starts_with(kProjectPrefix)) return raygen;
    return {};
}

std::vector<std::string> GraphCanvasModel::computeOpenSockets(std::string_view scope,
                                                              std::string_view node) const {
    std::vector<std::string> open;
    const auto *found = findNode(scope, node);
    if (found == nullptr || text(*found, "op") != "render.compute") return open;
    const auto parameters = found->value("parameters", Json::object());
    const auto known = shaderInterface(computeShaderStem(parameters));
    if (!known || !known->ready()) return open;
    const auto ports = parameters.is_object() ? parameters.value("ports", Json::object()) : Json::object();
    for (const auto &port : known->ports)
        if (port.type == "sampled_image" && !(ports.is_object() && ports.contains(port.port)))
            open.push_back(port.port);
    return open;
}

namespace {

// RTL-2s, decision 1: the ray tracing a project stem makes. Without a ray
// group layout, the stem's raygen, miss and closest hit; with one, the
// layout's form by the rule Enable hit groups uses (rayTracingInLayoutForm,
// RTL-2s-c): the first ray type traced, every miss and every group's cell
// the stem (a procedural group's intersection too), so the task the palette
// places is whole. The fields' grid changes each place afterwards.
Json rayTracingOfStem(const std::optional<RayLayoutRead> &read, const std::string &reference) {
    Json legacy{{"raygen", reference}, {"miss", reference}, {"closesthit", reference}};
    if (!read || !read->layout.usable() || rayLayoutRayTypes(read->layout.document).empty()) return legacy;
    return rayTracingInLayoutForm(legacy, read->layout.document, reference);
}

} // namespace

Json GraphCanvasModel::computeShaderOps(std::string_view scope, const std::string &node,
                                        const std::string &reference) const {
    const auto known = shaderInterface(reference);
    if (!known || !known->ready() || node.empty()) return Json::array();
    const auto *existing = findNode(scope, node);
    if (existing != nullptr && text(*existing, "op") != "render.compute") return Json::array();
    // A set with no compute stage traces; one with neither runs nothing --
    // unless the engine could not read it, which the node fields then say.
    const bool ray_tracing = !known->compute && known->ray_tracing;
    if (!known->compute && !known->ray_tracing && known->reflection_error.empty()) return Json::array();
    const auto token = scopeToken(scope);
    auto parameters = existing != nullptr ? existing->value("parameters", Json::object()) : Json::object();
    if (!parameters.is_object()) parameters = Json::object();
    auto old_ports = parameters.value("ports", Json::object());
    if (!old_ports.is_object()) old_ports = Json::object();
    auto old_inputs = existing != nullptr ? existing->value("inputs", Json::object()) : Json::object();
    if (!old_inputs.is_object()) old_inputs = Json::object();
    const auto kept = [&](const std::string &port) {
        return old_ports.contains(port) && old_inputs.contains(port);
    };

    Json images = Json::array();
    std::set<std::string> added_ids;
    const auto freeId = [&](const std::string &base) {
        const auto taken = [&](const std::string &candidate) {
            return findNode(scope, candidate) != nullptr || added_ids.contains(candidate);
        };
        auto chosen = base;
        for (int suffix = 2; taken(chosen) && suffix < 10000; ++suffix)
            chosen = base + "_" + std::to_string(suffix);
        added_ids.insert(chosen);
        return chosen;
    };
    // In the body of a loop over a view axis, an image is made at the
    // family's size, as the palette's render.image is.
    const auto family = viewAxisFamilyOf(scope);
    const auto extent = family.empty() ? std::nullopt : viewFamilyImageExtent(family);
    Json ports = Json::object();
    Json inputs = Json::object();
    Json outputs = Json::array();
    std::string first_written;
    for (const auto &port : known->ports) {
        if (port.type == "storage_image") {
            if (kept(port.port)) {
                ports[port.port] = old_ports.at(port.port);
                inputs[port.port] = old_inputs.at(port.port);
            } else {
                const auto image = freeId(node + "__" + port.port);
                Json image_parameters{{"type", sceneColorType()},
                                      {"format", port.format.empty() ? std::string{"R16G16B16A16_SFLOAT"}
                                                                     : port.format},
                                      {"extent_scale", 1.0},
                                      {"usage", Json::array({"STORAGE", "SAMPLED"})}};
                if (extent)
                    for (const auto &[key, value] : extent->items()) image_parameters[key] = value;
                images.push_back(Json{{"op", "add_node"}, {"scope", token},
                                      {"node", Json{{"id", image}, {"op", "render.image"},
                                                    {"parameters", std::move(image_parameters)},
                                                    {"outputs", Json::array({"resource"})}}}});
                ports[port.port] = Json{{"kind", "image"}, {"type", sceneColorType()},
                                        {"access", "write"}, {"binding", "storage"}};
                inputs[port.port] = Json{{"node", image}, {"port", "resource"}};
            }
            outputs.push_back(port.port);
            if (first_written.empty()) first_written = port.port;
        } else if (port.type == "sampled_image" && kept(port.port)) {
            // A sampled image already fed keeps its line; one not fed yet is
            // an empty socket (computeOpenSockets).
            ports[port.port] = old_ports.at(port.port);
            inputs[port.port] = old_inputs.at(port.port);
        }
    }

    parameters.erase("shader");
    parameters.erase("ray_tracing");
    if (ray_tracing)
        parameters["ray_tracing"] = rayTracingOfStem(ray_layout_read_, reference);
    else
        parameters["shader"] = reference;
    parameters["ports"] = ports;
    if (ray_tracing) {
        if (first_written.empty()) return Json::array();
        parameters["dispatch"] = Json{{"rays_from", Json{{"port", first_written}}}};
    } else if (!first_written.empty() && known->local_size[0] > 0 && known->local_size[2] == 1) {
        parameters["dispatch"] = Json{{"groups_from", Json{{"port", first_written}}}};
    } else {
        // Nothing written to count groups from: a fixed count, the one the
        // node had if it had one.
        const auto dispatch = parameters.value("dispatch", Json::object());
        if (!(dispatch.is_object() && dispatch.size() == 1 && dispatch.contains("groups")))
            parameters["dispatch"] = Json{{"groups", Json::array({1, 1, 1})}};
    }

    // A port the shader no longer declares loses the lines read from it.
    Json disconnects = Json::array();
    if (existing != nullptr) {
        if (const auto *nodes = scopeNodes(scope))
            for (const auto &reader : *nodes) {
                if (!reader.is_object()) continue;
                const auto reader_inputs = reader.value("inputs", Json::object());
                if (!reader_inputs.is_object()) continue;
                for (const auto &[reader_port, edge] : reader_inputs.items()) {
                    if (!edge.is_object() || text(edge, "node") != node) continue;
                    const auto port = text(edge, "port");
                    if (!old_ports.contains(port) || ports.contains(port)) continue;
                    auto ops = removeInputPortOps(scope, text(reader, "id"), reader_port);
                    if (ops.empty())
                        ops = Json::array({Json{{"op", "set_input"}, {"scope", token},
                                                {"node", text(reader, "id")}, {"port", reader_port},
                                                {"source", nullptr}}});
                    for (auto &op : ops) disconnects.push_back(std::move(op));
                }
            }
    }

    Json written = existing != nullptr ? *existing : Json{{"id", node}, {"op", "render.compute"}};
    written["parameters"] = std::move(parameters);
    if (inputs.empty()) written.erase("inputs");
    else written["inputs"] = std::move(inputs);
    written["outputs"] = std::move(outputs);
    if (existing != nullptr && written == *existing && images.empty() && disconnects.empty())
        return Json::array();
    Json ops = std::move(disconnects);
    for (auto &image : images) ops.push_back(std::move(image));
    ops.push_back(Json{{"op", existing != nullptr ? "replace_node" : "add_node"},
                       {"scope", token},
                       {"node", std::move(written)}});
    return ops;
}

bool GraphCanvasModel::setComputeShader(std::string_view scope, const std::string &node,
                                        const std::string &reference) {
    if (!canEdit() || reference.empty()) return false;
    const auto *found = findNode(scope, node);
    if (found == nullptr || text(*found, "op") != "render.compute") return false;
    if (!reference.starts_with(kProjectPrefix)) return setNodeParameter(scope, node, "shader", reference);
    const auto known = shaderInterface(reference);
    if (known && known->ready() && !shaderInterfaceWaiting(reference)) {
        auto ops = computeShaderOps(scope, node, reference);
        return !ops.empty() && submitOps(std::move(ops));
    }
    // Sent from poll() once the engine has read it.
    pending_compute_shader_ = PendingComputeShader{std::string{scope}, node, reference};
    askShaderInterface(reference);
    return true;
}

std::string GraphCanvasModel::reflectionErrorSentence(std::string_view reference,
                                                      std::string_view message) {
    auto first = std::string{message.substr(0, message.find('\n'))};
    while (!first.empty() && (first.back() == '\r' || first.back() == ' ')) first.pop_back();
    auto sentence = "The engine could not read " + std::string{reference} + "'s ports: " + first;
    if (message.find("undeclared identifier") != std::string_view::npos ||
        message.find("#error") != std::string_view::npos ||
        message.find("undefined macro") != std::string_view::npos)
        sentence += " (" + std::string{kReflectionDefinesHint} + ")";
    return sentence;
}

void GraphCanvasModel::refreshComputeShaderInterfaces() {
    std::map<std::string, std::vector<std::string>, std::less<>> stems;
    for (const auto &scope : scopes()) {
        const auto *nodes = scopeNodes(scope.id);
        if (nodes == nullptr) continue;
        for (const auto &node : *nodes) {
            if (!node.is_object() || text(node, "op") != "render.compute") continue;
            const auto parameters = node.value("parameters", Json::object());
            const auto stem = computeShaderStem(parameters);
            if (stem.empty()) continue;
            // Without an earlier answer, what the nodes declare is what the
            // new one is compared with.
            auto &declared = stems[stem];
            const auto ports = parameters.value("ports", Json::object());
            if (ports.is_object())
                for (const auto &[port, declaration] : ports.items()) {
                    (void)declaration;
                    if (std::ranges::find(declared, port) == declared.end()) declared.push_back(port);
                }
        }
    }
    for (auto &[stem, declared] : stems) {
        if (!shader_interface_refreshing_.contains(stem)) {
            const auto known = shaderInterface(stem);
            shader_interface_refreshing_[stem] =
                known && known->ready() ? imagePortNames(*known) : std::move(declared);
        }
        askShaderInterface(stem, true);
    }
}

void GraphCanvasModel::comparePortsAfterRefresh(const std::string &reference) {
    const auto baseline = std::move(shader_interface_refreshing_.at(reference));
    shader_interface_refreshing_.erase(reference);
    const auto known = shaderInterface(reference);
    if (!known || !known->ready()) return;
    const auto now = imagePortNames(*known);
    std::vector<std::string> added;
    std::vector<std::string> removed;
    for (const auto &port : now)
        if (std::ranges::find(baseline, port) == baseline.end()) added.push_back(port);
    for (const auto &port : baseline)
        if (std::ranges::find(now, port) == now.end()) removed.push_back(port);
    if (added.empty() && removed.empty()) return;
    std::erase_if(compute_port_changes_,
                  [&](const GraphCanvasPortChange &change) { return change.shader == reference; });
    for (const auto &scope : scopes()) {
        const auto *nodes = scopeNodes(scope.id);
        if (nodes == nullptr) continue;
        for (const auto &node : *nodes)
            if (node.is_object() && text(node, "op") == "render.compute" &&
                computeShaderStem(node.value("parameters", Json::object())) == reference)
                compute_port_changes_.push_back({scope.id, text(node, "id"), reference, added, removed});
    }
    if (std::ranges::none_of(compute_port_changes_, [&](const GraphCanvasPortChange &change) {
            return change.shader == reference;
        }))
        return;
    std::string line = reference + " changed its ports:";
    if (!added.empty()) line += " + " + joinedNames(added);
    if (!added.empty() && !removed.empty()) line += " /";
    if (!removed.empty()) line += " \xE2\x88\x92 " + joinedNames(removed);
    edit_notice_ = {InspectorNoticeKind::Information, std::move(line)};
    ++report_revision_;
}

bool GraphCanvasModel::updateComputePorts() {
    if (!canEdit() || compute_port_changes_.empty()) return false;
    Json ops = Json::array();
    for (const auto &change : compute_port_changes_)
        for (auto &op : computeShaderOps(change.scope, change.node, change.shader))
            ops.push_back(std::move(op));
    compute_port_changes_.clear();
    ++report_revision_;
    return !ops.empty() && submitOps(std::move(ops));
}

} // namespace PelicanStudio
