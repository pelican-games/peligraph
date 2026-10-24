#include "renderingjsonnormalize.hpp"

#include "graphcomposition.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>

namespace Pelican {

namespace {

using Json = nlohmann::json;

std::atomic<int> &bypassDepth() {
    static std::atomic<int> depth{0};
    return depth;
}

std::string pointerToken(std::string_view key) {
    std::string token;
    token.reserve(key.size());
    for (const char ch : key) {
        if (ch == '~') token += "~0";
        else if (ch == '/') token += "~1";
        else token += ch;
    }
    return token;
}

std::string child(const std::string &path, std::string_view key) { return path + "/" + pointerToken(key); }
std::string child(const std::string &path, std::size_t index) { return path + "/" + std::to_string(index); }

GraphRegistryError notNormalized(const std::string &path) {
    return GraphRegistryError(std::string{renderJsonNotNormalizedCode}, Json{{"path", path}});
}

std::optional<std::string> canonicalLoadOp(const std::string &spelling) {
    if (spelling == "Clear" || spelling == "clear") return "clear";
    if (spelling == "Load" || spelling == "load") return "load";
    if (spelling == "DontCare" || spelling == "dont_care" || spelling == "dontCare") return "dont_care";
    return std::nullopt;
}

std::optional<std::string> canonicalStoreOp(const std::string &spelling) {
    if (spelling == "Store" || spelling == "store") return "store";
    if (spelling == "DontCare" || spelling == "dont_care" || spelling == "dontCare") return "dont_care";
    return std::nullopt;
}

constexpr std::array<const char *, 5> name_list_keys{"input", "reads", "writes", "after", "before"};

// One walk for both directions: `check` refuses (json_not_normalized) where
// the normalizing walk would change or refuse a value.
struct Walker {
    bool check = false;

    void replace(Json &slot, Json value, const std::string &path) const {
        if (check) throw notNormalized(path);
        slot = std::move(value);
    }

    [[noreturn]] void invalid(const std::string &path, std::string_view expected) const {
        if (check) throw notNormalized(path);
        throw GraphRegistryError(std::string{renderJsonShapeInvalidCode}, Json{{"path", path}, {"expected", expected}});
    }

    void nameList(Json &holder, std::string_view key, const std::string &path) const {
        const auto found = holder.find(key);
        if (found == holder.end()) return;
        const auto at = child(path, key);
        auto &value = *found;
        if (value.is_null()) return replace(value, Json::array(), at);
        if (value.is_string()) return replace(value, Json::array({value}), at);
        if (!value.is_array()) invalid(at, "a string or an array of strings");
        for (std::size_t index = 0; index < value.size(); ++index)
            if (!value.at(index).is_string()) invalid(child(at, index), "a string");
    }

    void opSpelling(Json &holder, std::string_view key, bool load, const std::string &path) const {
        const auto found = holder.find(key);
        if (found == holder.end() || !found->is_string()) return;
        const auto spelling = found->get<std::string>();
        const auto canonical = load ? canonicalLoadOp(spelling) : canonicalStoreOp(spelling);
        // An unknown spelling stays for the core parser to refuse.
        if (canonical && *canonical != spelling) replace(*found, *canonical, child(path, key));
    }

    void attachment(Json &value, const std::string &path, std::string_view expected) const {
        if (value.is_string()) return replace(value, Json{{"target", value}}, path);
        if (!value.is_object()) invalid(path, expected);
        const auto target = value.find("target");
        if (target == value.end() || !target->is_string()) invalid(child(path, "target"), "a render target name");
        opSpelling(value, "load_op", true, path);
        opSpelling(value, "store_op", false, path);
    }

    void output(Json &pass, const std::string &path) const {
        const auto found = pass.find("output");
        if (found == pass.end() || !found->is_object()) return;
        auto &output = *found;
        const auto at = child(path, "output");
        constexpr std::string_view color_expected = "a render target name or an attachment object";
        if (const auto color = output.find("color"); color != output.end()) {
            const auto color_at = child(at, "color");
            if (color->is_null()) {
                replace(*color, Json::array(), color_at);
            } else if (!color->is_array()) {
                if (check) throw notNormalized(color_at);
                auto single = *color;
                attachment(single, color_at, color_expected);
                *color = Json::array({std::move(single)});
            } else {
                for (std::size_t index = 0; index < color->size(); ++index)
                    attachment(color->at(index), child(color_at, index), color_expected);
            }
        }
        if (const auto depth = output.find("depth"); depth != output.end() && !depth->is_null()) {
            const auto depth_at = child(at, "depth");
            // An empty list is no depth attachment, as the core parser reads
            // it; a list of one is not a form the pass shape takes.
            if (depth->is_array() && depth->empty()) replace(*depth, nullptr, depth_at);
            else attachment(*depth, depth_at, "null, a render target name or an attachment object");
        }
    }

    void declaration(Json &value, const std::string &path) const {
        if (!value.is_object()) return;
        for (const auto *key : name_list_keys) nameList(value, key, path);
        output(value, path);
        opSpelling(value, "color_load_op", true, path);
        opSpelling(value, "depth_load_op", true, path);
        opSpelling(value, "color_store_op", false, path);
        opSpelling(value, "depth_store_op", false, path);
    }

    void tasks(Json &holder, const std::string &path) const {
        const auto found = holder.find("compute_tasks");
        if (found == holder.end() || !found->is_array()) return;
        const auto at = child(path, "compute_tasks");
        for (std::size_t index = 0; index < found->size(); ++index) declaration(found->at(index), child(at, index));
    }

    void buffers(Json &holder, const std::string &path) const {
        const auto found = holder.find("buffers");
        if (found == holder.end() || !found->is_array()) return;
        const auto at = child(path, "buffers");
        for (std::size_t index = 0; index < found->size(); ++index) {
            auto &entry = found->at(index);
            if (entry.is_string()) replace(entry, Json{{"name", entry}}, child(at, index));
            else if (!entry.is_object()) invalid(child(at, index), "a buffer name or a buffer object");
        }
    }

    void config(Json &value, const std::string &path) const {
        if (!value.is_object()) return;
        if (const auto families = value.find("rendering_passes"); families != value.end() && families->is_array()) {
            const auto families_at = child(path, "rendering_passes");
            for (std::size_t family = 0; family < families->size(); ++family) {
                auto &entry = families->at(family);
                if (!entry.is_object()) continue;
                const auto family_at = child(families_at, family);
                if (const auto passes = entry.find("passes"); passes != entry.end() && passes->is_array()) {
                    const auto passes_at = child(family_at, "passes");
                    for (std::size_t index = 0; index < passes->size(); ++index)
                        declaration(passes->at(index), child(passes_at, index));
                }
                tasks(entry, family_at);
                buffers(entry, family_at);
            }
        }
        tasks(value, path);
        buffers(value, path);
    }

    // A feature, or one of its integrations' fragments.
    void featureFragment(Json &value, const std::string &path) const {
        if (!value.is_object()) return;
        if (const auto passes = value.find("passes"); passes != value.end() && passes->is_array()) {
            const auto passes_at = child(path, "passes");
            for (std::size_t index = 0; index < passes->size(); ++index) {
                auto &entry = passes->at(index);
                if (!entry.is_object()) continue;
                if (const auto pass = entry.find("pass"); pass != entry.end())
                    declaration(*pass, child(child(passes_at, index), "pass"));
            }
        }
        tasks(value, path);
        buffers(value, path);
        if (const auto overrides = value.find("pass_overrides"); overrides != value.end() && overrides->is_object()) {
            const auto overrides_at = child(path, "pass_overrides");
            for (auto &[name, override_json] : overrides->items())
                if (override_json.is_object()) nameList(override_json, "input", child(overrides_at, name));
        }
    }

    void feature(Json &value) const {
        if (!value.is_object()) return;
        featureFragment(value, "");
        if (const auto integrations = value.find("integrations");
            integrations != value.end() && integrations->is_array()) {
            for (std::size_t index = 0; index < integrations->size(); ++index) {
                auto &integration = integrations->at(index);
                if (!integration.is_object()) continue;
                if (const auto fragment = integration.find("fragment"); fragment != integration.end())
                    featureFragment(*fragment, child(child("/integrations", index), "fragment"));
            }
        }
    }

    void document(Json &value) const {
        if (!value.is_object()) return;
        const auto schema = value.find("schema");
        const auto name = schema != value.end() && schema->is_string() ? schema->get<std::string>() : std::string{};
        if (name == "pelican.graph") {
            if (const auto rendering = value.find("rendering"); rendering != value.end())
                config(*rendering, "/rendering");
        } else if (name == "pelican.render_feature") {
            feature(value);
        } else if (name == "pelican.render_pipeline") {
            if (const auto config_json = value.find("config"); config_json != value.end())
                config(*config_json, "/config");
        } else {
            config(value, "");
        }
    }
};

// The name a reader's refusal says the value belongs to.
std::string declarationPath(const Json &declaration, std::string_view rest) {
    std::string name = "<unnamed>";
    if (declaration.is_object())
        if (const auto found = declaration.find("name"); found != declaration.end() && found->is_string())
            name = found->get<std::string>();
    return pointerToken(name) + std::string{rest};
}

const Json *attachmentsOf(const Json &pass, std::string_view key) {
    if (!pass.is_object()) return nullptr;
    const auto output = pass.find("output");
    if (output == pass.end() || !output->is_object()) return nullptr;
    const auto found = output->find(key);
    return found == output->end() ? nullptr : &*found;
}

const std::string &attachmentTarget(const Json &pass, const Json &attachment, const std::string &rest) {
    if (!attachment.is_object()) throw notNormalized(declarationPath(pass, rest));
    const auto target = attachment.find("target");
    if (target == attachment.end() || !target->is_string()) throw notNormalized(declarationPath(pass, rest + "/target"));
    return target->get_ref<const std::string &>();
}

} // namespace

nlohmann::json normalizeRenderingJson(nlohmann::json document) {
    if (bypassDepth().load() > 0) return document;
    Walker{}.document(document);
    return document;
}

nlohmann::json normalizeRenderFeatureJson(nlohmann::json feature) {
    if (bypassDepth().load() > 0) return feature;
    Walker{}.feature(feature);
    return feature;
}

void requireNormalizedRenderingJson(const nlohmann::json &document) {
    // The walk never writes in this direction (it refuses first); the copy
    // keeps the signature honest.
    auto copy = document;
    Walker{true}.document(copy);
}

nlohmann::json normalizeProviderRenderingJson(nlohmann::json response) {
    auto normalized = normalizeRenderingJson(std::move(response));
    requireNormalizedRenderingJson(normalized);
    return normalized;
}

ScopedRenderingJsonNormalizationBypass::ScopedRenderingJsonNormalizationBypass() noexcept { ++bypassDepth(); }
ScopedRenderingJsonNormalizationBypass::~ScopedRenderingJsonNormalizationBypass() { --bypassDepth(); }

namespace RenderJson {

std::vector<std::string> names(const nlohmann::json &declaration, std::string_view key) {
    std::vector<std::string> result;
    if (!declaration.is_object()) return result;
    const auto found = declaration.find(key);
    if (found == declaration.end()) return result;
    if (!found->is_array()) throw notNormalized(declarationPath(declaration, "/" + std::string{key}));
    result.reserve(found->size());
    for (std::size_t index = 0; index < found->size(); ++index) {
        const auto &entry = found->at(index);
        if (!entry.is_string())
            throw notNormalized(declarationPath(declaration, "/" + std::string{key} + "/" + std::to_string(index)));
        result.push_back(entry.get<std::string>());
    }
    return result;
}

bool namesContain(const nlohmann::json &declaration, std::string_view key, std::string_view name) {
    const auto values = names(declaration, key);
    return std::find(values.begin(), values.end(), name) != values.end();
}

void appendName(nlohmann::json &declaration, std::string_view key, const std::string &name) {
    if (!declaration.contains(key)) declaration[std::string{key}] = Json::array();
    if (namesContain(declaration, key, name)) return;
    declaration.at(key).push_back(name);
}

std::vector<std::string> colorTargets(const nlohmann::json &pass) {
    std::vector<std::string> result;
    const auto *colors = attachmentsOf(pass, "color");
    if (colors == nullptr) return result;
    if (!colors->is_array()) throw notNormalized(declarationPath(pass, "/output/color"));
    for (std::size_t index = 0; index < colors->size(); ++index)
        result.push_back(attachmentTarget(pass, colors->at(index), "/output/color/" + std::to_string(index)));
    return result;
}

std::optional<std::string> depthTarget(const nlohmann::json &pass) {
    const auto *depth = attachmentsOf(pass, "depth");
    if (depth == nullptr || depth->is_null()) return std::nullopt;
    return attachmentTarget(pass, *depth, "/output/depth");
}

std::vector<std::string> outputTargets(const nlohmann::json &pass) {
    auto result = colorTargets(pass);
    if (auto depth = depthTarget(pass)) result.push_back(std::move(*depth));
    return result;
}

bool writesOutputTarget(const nlohmann::json &pass, std::string_view target) {
    const auto targets = outputTargets(pass);
    return std::find(targets.begin(), targets.end(), target) != targets.end();
}

std::vector<std::string> bufferNames(const nlohmann::json &holder) {
    std::vector<std::string> result;
    if (!holder.is_object()) return result;
    const auto found = holder.find("buffers");
    if (found == holder.end()) return result;
    if (!found->is_array()) throw notNormalized("/buffers");
    for (std::size_t index = 0; index < found->size(); ++index) {
        const auto &entry = found->at(index);
        if (!entry.is_object()) throw notNormalized("/buffers/" + std::to_string(index));
        const auto name = entry.find("name");
        if (name != entry.end() && name->is_string()) result.push_back(name->get<std::string>());
    }
    return result;
}

void retargetColorOutputs(nlohmann::json &pass, const TargetRename &rename) {
    (void)colorTargets(pass);
    const auto *colors = attachmentsOf(pass, "color");
    if (colors == nullptr) return;
    for (auto &entry : pass.at("output").at("color")) {
        auto &target = entry.at("target");
        if (auto renamed = rename(target.get<std::string>())) target = std::move(*renamed);
    }
}

void retargetOutputs(nlohmann::json &pass, const TargetRename &rename) {
    retargetColorOutputs(pass, rename);
    if (!depthTarget(pass)) return;
    auto &target = pass.at("output").at("depth").at("target");
    if (auto renamed = rename(target.get<std::string>())) target = std::move(*renamed);
}

void retargetNames(nlohmann::json &declaration, std::string_view key, const TargetRename &rename) {
    (void)names(declaration, key);
    if (!declaration.is_object() || !declaration.contains(key)) return;
    for (auto &entry : declaration.at(key))
        if (auto renamed = rename(entry.get<std::string>())) entry = std::move(*renamed);
}

nlohmann::json attachment(std::string target) { return Json{{"target", std::move(target)}}; }

} // namespace RenderJson

} // namespace Pelican
