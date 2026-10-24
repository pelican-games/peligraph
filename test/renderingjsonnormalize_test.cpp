// RC-3b: the rendering JSON's one normal form (renderingjsonnormalize.hpp),
// applied at every boundary and read by one set of target-reference helpers.
#include "../src/core/imgui/imguiruntime.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/registrationownerhost.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerprogramdetail.hpp"
#include "../src/project/authoringlowering.hpp"
#include "../src/project/featurecompose.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphvariantpolicy.hpp"
#include "../src/project/projecttemplate.hpp"
#include "../src/project/renderingjsonnormalize.hpp"
#include "../src/project/renderpipeline.hpp"
#include "render_graph_root_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

std::string loadEngineDocument(std::string_view ref) {
    constexpr std::string_view prefix = "engine://";
    REQUIRE(ref.starts_with(prefix));
    return engineResourceOrThrow(ref.substr(prefix.size()));
}

Json att(const std::string &target) { return Json{{"target", target}}; }

// The named code and detail a call refuses with.
struct Refusal {
    std::string code;
    Json detail;
};

Refusal refusalOf(const std::function<void()> &call) {
    try {
        call();
    } catch (const GraphRegistryError &error) {
        return {error.code(), error.detail()};
    } catch (const std::exception &error) {
        FAIL("refused without a named code: " << error.what());
    }
    FAIL("expected a refusal");
    return {};
}

// --- B1's project -----------------------------------------------------------

// Which document writes the short forms; the others write the normal form.
enum class Shorthand { none, root, feature, provider };

Json image(const std::string &name) {
    return Json{{"name", name},
                {"extent_scale", 1.0},
                {"format", "R8G8B8A8_UNORM"},
                {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
}

// The root: `draw` writes `scene`; the feature runs after it; `present`
// reads what the provider's pass writes and draws to the swapchain.
Json b1Root(bool shorthand, const Json &extra_present = Json::object()) {
    auto atlas = image("atlas");
    atlas["mip_levels"] = 3;
    atlas["layers"] = 2;
    Json present{{"name", "present"}, {"type", "fullscreen"}};
    if (shorthand) {
        present["input"] = "post";
        present["after"] = "draw";
        present["output"] = Json{{"color", "swapchain"}, {"depth", nullptr}};
    } else {
        present["input"] = Json::array({"post"});
        present["after"] = Json::array({"draw"});
        present["output"] = Json{{"color", Json::array({att("swapchain")})}, {"depth", nullptr}};
    }
    for (const auto &[key, value] : extra_present.items()) present[key] = value;
    Json draw{{"name", "draw"},
              {"type", "fullscreen"},
              {"output", shorthand ? Json{{"color", "scene"}, {"depth", nullptr}}
                                   : Json{{"color", Json::array({att("scene")})}, {"depth", nullptr}}}};
    return TestSupport::renderGraphRoot(Json{
        {"render_targets", Json::array({image("scene"), atlas, image("post"), image("post2")})},
        {"features", Json::array({Json{{"ref", "project://features/rc3b.json"},
                                       {"parameters", {{"dst", "atlas"}}}}})},
        {"graph_transforms", Json::array({Json{{"name", "rc3b.transform"},
                                               {"parameters", Json::object()},
                                               {"provider", "rc3b.provider"}}})},
        {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({draw, present})}}})},
    });
}

// The feature: `to_atlas` writes the bound `$dst` at mip 2, layer 1, its
// attachment's ops differing from the pass's; `to_post2` writes post2 with
// the pass's ops only; `from_atlas` reads `$dst` and writes `post` with
// neither (the defaults, clear / store).
Json b1Feature(bool shorthand) {
    Json to_atlas{{"name", "to_atlas"},
                  {"type", "fullscreen"},
                  {"color_load_op", "load"},
                  {"color_store_op", "dont_care"}};
    const Json atlas_attachment{{"target", "$dst"},
                                {"subresource", {{"mip", 2}, {"layer", 1}}},
                                {"load_op", shorthand ? "DontCare" : "dont_care"},
                                {"store_op", "store"}};
    Json to_post2{{"name", "to_post2"},
                  {"type", "fullscreen"},
                  {"color_load_op", shorthand ? "Load" : "load"},
                  {"color_store_op", "dont_care"}};
    Json from_atlas{{"name", "from_atlas"}, {"type", "fullscreen"}};
    if (shorthand) {
        to_atlas["input"] = "scene";
        to_atlas["output"] = Json{{"color", atlas_attachment}, {"depth", nullptr}};
        to_post2["output"] = Json{{"color", "post2"}, {"depth", nullptr}};
        from_atlas["input"] = "$dst";
        from_atlas["output"] = Json{{"color", "post"}, {"depth", nullptr}};
    } else {
        to_atlas["input"] = Json::array({"scene"});
        to_atlas["output"] = Json{{"color", Json::array({atlas_attachment})}, {"depth", nullptr}};
        to_post2["output"] = Json{{"color", Json::array({att("post2")})}, {"depth", nullptr}};
        from_atlas["input"] = Json::array({"$dst"});
        from_atlas["output"] = Json{{"color", Json::array({att("post")})}, {"depth", nullptr}};
    }
    return Json{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "rc3b"},
        {"runtime_shader_compiler", "optional"},
        {"parameters",
         {{"schema", "pelican.render_feature_parameters"},
          {"version", 1},
          {"render_targets", Json::array({Json{{"name", "dst"}, {"required", true}}})}}},
        {"passes", Json::array({Json{{"insert", "after:draw"}, {"pass", to_atlas}},
                                Json{{"insert", "after:to_atlas"}, {"pass", to_post2}},
                                Json{{"insert", "after:to_post2"}, {"pass", from_atlas}}})},
    };
}

// The provider adds `blurred` and `provider_pass` (reads post, after
// from_atlas) and has present read `blurred` after it -- in the short forms
// when `shorthand`.
struct ProviderState {
    bool shorthand = false;
    // Replaces provider_pass's after with this (B1's mixed entry).
    Json provider_after;
    std::string response;
};

RenderGraphTransform::Status resolveB1Transform(
    void *context, const RenderGraphTransform::ResolveGraphTransformInputV1 *input,
    RenderGraphTransform::GraphTransformOutputV1 *output) noexcept {
    auto &state = *static_cast<ProviderState *>(context);
    try {
        auto config = Json::parse(input->config_json_utf8, input->config_json_utf8 + input->config_json_size);
        config["render_targets"].push_back(image("blurred"));
        auto &passes = config["rendering_passes"][0]["passes"];
        const auto present = std::find_if(passes.begin(), passes.end(),
                                          [](const Json &pass) { return pass.value("name", "") == "present"; });
        if (present == passes.end()) return RenderGraphTransform::Status::provider_error;
        if (state.shorthand) {
            (*present)["input"] = "blurred";
            (*present)["after"] = "provider_pass";
        } else {
            (*present)["input"] = Json::array({"blurred"});
            (*present)["after"] = Json::array({"provider_pass"});
        }
        Json pass{{"name", "provider_pass"}, {"type", "fullscreen"}};
        if (state.shorthand) {
            pass["input"] = "post";
            pass["after"] = "from_atlas";
            pass["output"] = Json{{"color", "blurred"}, {"depth", nullptr}};
        } else {
            pass["input"] = Json::array({"post"});
            pass["after"] = Json::array({"from_atlas"});
            pass["output"] = Json{{"color", Json::array({att("blurred")})}, {"depth", nullptr}};
        }
        if (!state.provider_after.is_null()) pass["after"] = state.provider_after;
        passes.insert(present, std::move(pass));
        state.response = config.dump();
    } catch (...) {
        return RenderGraphTransform::Status::provider_error;
    }
    static const std::string implementation = "rc3b.transform@1";
    *output = RenderGraphTransform::descriptor<RenderGraphTransform::GraphTransformOutputV1>();
    output->implementation_id_utf8 = implementation.data();
    output->implementation_id_size = static_cast<std::uint32_t>(implementation.size());
    output->config_json_utf8 = state.response.data();
    output->config_json_size = static_cast<std::uint32_t>(state.response.size());
    return RenderGraphTransform::Status::ok;
}

struct B1Project {
    std::filesystem::path root;
    PathResolver paths;
    GraphTransformRegistry registry;
    ProviderState state;
    RenderGraphTransform::ProviderHandleV1 handle{};
    Json config;

    explicit B1Project(Shorthand shorthand, const Json &extra_present = Json::object()) {
        root = std::filesystem::temp_directory_path() /
               ("pelican-rc3b-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "features");
        paths.setup(root, false);
        std::ofstream{root / "features" / "rc3b.json", std::ios::binary}
            << b1Feature(shorthand == Shorthand::feature).dump(2);
        config = b1Root(shorthand == Shorthand::root, extra_present);
        state.shorthand = shorthand == Shorthand::provider;
        static constexpr std::string_view name = "rc3b.provider";
        auto provider = RenderGraphTransform::descriptor<RenderGraphTransform::ProviderV1>();
        provider.capability_bits = RenderGraphTransform::builtinProviderCapabilitiesV1;
        provider.name_utf8 = name.data();
        provider.name_size = static_cast<std::uint32_t>(name.size());
        provider.context = &state;
        provider.resolve_graph_transform = resolveB1Transform;
        const auto owner = internal::allocateRegistrationOwner();
        REQUIRE(registry.registerProvider(provider, owner, handle) == RenderGraphTransform::Status::ok);
        registry.activateOwner(owner);
    }
    B1Project(const B1Project &) = delete;
    B1Project &operator=(const B1Project &) = delete;
    ~B1Project() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    // The production compiler program, as the registration runs it, without
    // a device (compiler_only).
    RenderCompilerProgramVariantOutput compile() {
        const auto transforms = registry.snapshot();
        const RenderStrategyRegistrySnapshot strategies;
        const SubgraphReplacementRegistrySnapshot subgraphs;
        const VulkanRenderCompilerBackendContext backend{
            vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
            VulkanRenderCompilerDevicePlanningMode::compiler_only};
        const std::vector<RenderCompilerProgramVariantRequest> variants{
            {.graph_variant = RenderPipelineGraphVariant::flat}};
        auto output = runRenderCompilerProgram(
            defaultVulkanRenderCompilerProgram(),
            RenderCompilerProgramInput{
                .rendering_config = config,
                .source_name = "rc3b",
                .path_resolver = paths,
                .graph_transforms = transforms,
                .render_strategies = strategies,
                .subgraph_replacements = subgraphs,
                .backend_context = backend,
                .variants = variants,
            });
        REQUIRE(output.variants.size() == 1);
        return std::move(output.variants.front());
    }
};

const FramePlanNode &planNode(const FramePlan &plan, const std::string &name) {
    const auto found = std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                    [&](const FramePlanNode &node) { return node.name == name; });
    REQUIRE(found != plan.nodes.end());
    return *found;
}

bool lists(const std::vector<std::string> &values, const std::string &value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

// What B1 resolves with the normalization in place, whichever document wrote
// the short forms.
void requireB1Resolution(const RenderCompilerProgramVariantOutput &variant) {
    const auto &plan = variant.frame_plans.at("main");
    // FramePlan order.
    std::vector<std::string> order;
    for (const auto &node : plan.nodes)
        if (lists({"draw", "to_atlas", "to_post2", "from_atlas", "provider_pass", "present", "output_transform"},
                  node.name))
            order.push_back(node.name);
    CHECK(order == std::vector<std::string>{"draw", "to_atlas", "to_post2", "from_atlas", "provider_pass", "present",
                                            "output_transform"});
    // Writer -> reader: each read is of what the pass before it wrote. `$dst`
    // is the parameter's target (atlas); swapchain is display.
    CHECK(lists(planNode(plan, "draw").writes, "scene"));
    CHECK(lists(planNode(plan, "to_atlas").reads, "scene"));
    CHECK(lists(planNode(plan, "to_atlas").writes, "atlas"));
    CHECK(lists(planNode(plan, "to_post2").writes, "post2"));
    CHECK(lists(planNode(plan, "from_atlas").reads, "atlas"));
    CHECK(lists(planNode(plan, "from_atlas").writes, "post"));
    CHECK(lists(planNode(plan, "provider_pass").reads, "post"));
    CHECK(lists(planNode(plan, "provider_pass").writes, "blurred"));
    CHECK(lists(planNode(plan, "present").reads, "blurred"));
    CHECK(lists(planNode(plan, "present").writes, "display"));
    CHECK_FALSE(lists(planNode(plan, "present").writes, "swapchain"));
    CHECK(lists(planNode(plan, "output_transform").reads, "display"));
    // The composed declarations: after / input as lists, the attachments.
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(variant.normalized_config);
    REQUIRE(graphs.size() == 1);
    const auto node = [&](const std::string &name) -> const FrameGraphNodeDefinition & {
        const auto found = std::find_if(graphs.front().nodes.begin(), graphs.front().nodes.end(),
                                        [&](const auto &candidate) { return candidate.name == name; });
        REQUIRE(found != graphs.front().nodes.end());
        return *found;
    };
    CHECK(lists(node("present").after, "provider_pass"));
    CHECK(lists(node("provider_pass").after, "from_atlas"));
    const auto &to_atlas = node("to_atlas").attachments;
    REQUIRE(to_atlas.size() == 1);
    // The bound parameter target keeps the attachment's subresource and its
    // own load / store (attachment > pass).
    CHECK(to_atlas.at(0).resource == "atlas");
    REQUIRE(to_atlas.at(0).subresource.has_value());
    CHECK(to_atlas.at(0).subresource->base_mip_level == 2);
    CHECK(to_atlas.at(0).subresource->level_count == 1);
    CHECK(to_atlas.at(0).subresource->base_array_layer == 1);
    CHECK(to_atlas.at(0).subresource->layer_count == 1);
    CHECK(to_atlas.at(0).operations.load_op == AttachmentLoadOp::dont_care);
    CHECK(to_atlas.at(0).operations.store_op == AttachmentStoreOp::store);
    // An attachment without its own ops takes the pass's (pass > default).
    const auto &to_post2 = node("to_post2").attachments;
    REQUIRE(to_post2.size() == 1);
    CHECK(to_post2.at(0).resource == "post2");
    CHECK_FALSE(to_post2.at(0).subresource.has_value());
    CHECK(to_post2.at(0).operations.load_op == AttachmentLoadOp::load);
    CHECK(to_post2.at(0).operations.store_op == AttachmentStoreOp::dont_care);
    // A pass with none takes the defaults (clear / store).
    const auto &from_atlas = node("from_atlas").attachments;
    REQUIRE(from_atlas.size() == 1);
    CHECK(from_atlas.at(0).resource == "post");
    CHECK(from_atlas.at(0).operations.load_op == AttachmentLoadOp::clear);
    CHECK(from_atlas.at(0).operations.store_op == AttachmentStoreOp::store);
    // The swapchain output is display; the composed config is in the normal
    // form throughout.
    REQUIRE_NOTHROW(requireNormalizedRenderingJson(variant.normalized_config));
}

} // namespace

TEST_CASE("RC-3b the normal form: lists, attachments, buffers and op spellings, idempotent",
          "[rc3b][normalize]") {
    const Json shorthand{
        {"buffers", Json::array({"scratch", Json{{"name", "sized"}, {"size", 16}}})},
        {"compute_tasks", Json::array({Json{{"name", "task"}, {"reads", "a"}, {"writes", "b"}, {"after", "x"}}})},
        {"rendering_passes",
         Json::array({Json{{"name", "main"},
                           {"buffers", Json::array({"local"})},
                           {"passes", Json::array({Json{{"name", "p"},
                                                        {"type", "fullscreen"},
                                                        {"input", "a"},
                                                        {"after", nullptr},
                                                        {"before", "q"},
                                                        {"color_load_op", "Load"},
                                                        {"depth_store_op", "DontCare"},
                                                        {"shader", {{"defines", Json::array({"swapchain"})}}},
                                                        {"output",
                                                         {{"color", Json{{"target", "rt"}, {"load_op", "Clear"}}},
                                                          {"depth", "d"}}}},
                                                   Json{{"name", "q"},
                                                        {"type", "fullscreen"},
                                                        {"output", {{"color", nullptr}, {"depth", Json::array()}}}}})}}})},
    };
    const Json expected{
        {"buffers", Json::array({Json{{"name", "scratch"}}, Json{{"name", "sized"}, {"size", 16}}})},
        {"compute_tasks", Json::array({Json{{"name", "task"},
                                            {"reads", Json::array({"a"})},
                                            {"writes", Json::array({"b"})},
                                            {"after", Json::array({"x"})}}})},
        {"rendering_passes",
         Json::array({Json{{"name", "main"},
                           {"buffers", Json::array({Json{{"name", "local"}}})},
                           {"passes", Json::array({Json{{"name", "p"},
                                                        {"type", "fullscreen"},
                                                        {"input", Json::array({"a"})},
                                                        {"after", Json::array()},
                                                        {"before", Json::array({"q"})},
                                                        {"color_load_op", "load"},
                                                        {"depth_store_op", "dont_care"},
                                                        {"shader", {{"defines", Json::array({"swapchain"})}}},
                                                        {"output",
                                                         {{"color", Json::array({Json{{"target", "rt"},
                                                                                      {"load_op", "clear"}}})},
                                                          {"depth", att("d")}}}},
                                                   Json{{"name", "q"},
                                                        {"type", "fullscreen"},
                                                        {"output", {{"color", Json::array()}, {"depth", nullptr}}}}})}}})},
    };
    const auto normalized = normalizeRenderingJson(shorthand);
    CHECK(normalized == expected);
    CHECK(normalizeRenderingJson(normalized) == normalized);
    REQUIRE_NOTHROW(requireNormalizedRenderingJson(normalized));
    const auto refused = refusalOf([&] { requireNormalizedRenderingJson(shorthand); });
    CHECK(refused.code == std::string{renderJsonNotNormalizedCode});
    CHECK(refused.detail == Json{{"path", "/rendering_passes/0/passes/0/input"}});

    // A feature: its passes, overrides and integration fragments; a preset:
    // its config; a graph root: its rendering section.
    const Json feature{{"schema", "pelican.render_feature"},
                       {"passes", Json::array({Json{{"insert", "end"}, {"pass", {{"name", "f"}, {"input", "a"}}}}})},
                       {"pass_overrides", {{"p", {{"input", "b"}}}}},
                       {"integrations", Json::array({Json{{"fragment", {{"buffers", Json::array({"s"})}}}}})}};
    const auto normalized_feature = normalizeRenderingJson(feature);
    CHECK(normalized_feature.at("passes").at(0).at("pass").at("input") == Json::array({"a"}));
    CHECK(normalized_feature.at("pass_overrides").at("p").at("input") == Json::array({"b"}));
    CHECK(normalized_feature.at("integrations").at(0).at("fragment").at("buffers") ==
          Json::array({Json{{"name", "s"}}}));
    const Json preset{{"schema", "pelican.render_pipeline"}, {"config", shorthand}};
    CHECK(normalizeRenderingJson(preset).at("config") == expected);
    CHECK(normalizeRenderingJson(TestSupport::renderGraphRoot(shorthand)).at("rendering") == expected);
}

TEST_CASE("RC-3b what the normal form cannot be made from is refused by name and position",
          "[rc3b][normalize][refusal]") {
    const auto passes = [](Json pass) {
        return Json{{"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({pass})}}})}};
    };
    const auto shape = [](const Json &document) {
        return refusalOf([&] { (void)normalizeRenderingJson(document); });
    };
    auto refused = shape(passes(Json{{"name", "p"}, {"output", {{"color", Json::array({"rt", 7})}, {"depth", nullptr}}}}));
    CHECK(refused.code == std::string{renderJsonShapeInvalidCode});
    CHECK(refused.detail.at("path") == "/rendering_passes/0/passes/0/output/color/1");
    refused = shape(passes(Json{{"name", "p"}, {"after", Json::array({"producer", 7})}}));
    CHECK(refused.code == std::string{renderJsonShapeInvalidCode});
    CHECK(refused.detail.at("path") == "/rendering_passes/0/passes/0/after/1");
    refused = shape(passes(Json{{"name", "p"}, {"output", {{"color", nullptr}, {"depth", Json::array({"d"})}}}}));
    CHECK(refused.detail.at("path") == "/rendering_passes/0/passes/0/output/depth");
    refused = shape(Json{{"buffers", Json::array({3})}});
    CHECK(refused.detail.at("path") == "/buffers/0");
    // The readers refuse what skipped the normalization, by name.
    refused = refusalOf([] { (void)RenderJson::colorTargets(Json{{"name", "p"}, {"output", {{"color", "rt"}}}}); });
    CHECK(refused.code == std::string{renderJsonNotNormalizedCode});
    CHECK(refused.detail == Json{{"path", "p/output/color"}});
    refused = refusalOf([] { (void)RenderJson::names(Json{{"name", "p"}, {"after", "q"}}, "after"); });
    CHECK(refused.detail == Json{{"path", "p/after"}});
}

TEST_CASE("RC-3b target rewrites change only an attachment's target",
          "[rc3b][normalize][rewrite]") {
    auto pass = normalizeRenderingJson(Json{
        {"rendering_passes",
         Json::array({Json{{"name", "main"},
                           {"passes", Json::array({Json{
                                          {"name", "p"},
                                          {"input", Json::array({"swapchain_like", "$dst"})},
                                          {"output",
                                           {{"color", Json::array({Json{{"target", "$dst"},
                                                                        {"subresource", {{"mip", 1}, {"layer", 2}}},
                                                                        {"load_op", "load"},
                                                                        {"store_op", "dont_care"}}})},
                                            {"depth", "$dst"}}}}})}}})}})["rendering_passes"][0]["passes"][0];
    const RenderJson::TargetRename bind = [](const std::string &target) -> std::optional<std::string> {
        if (target == "$dst") return std::string{"atlas"};
        return std::nullopt;
    };
    RenderJson::retargetOutputs(pass, bind);
    RenderJson::retargetNames(pass, "input", bind);
    CHECK(pass.at("output").at("color") == Json::array({Json{{"target", "atlas"},
                                                             {"subresource", {{"mip", 1}, {"layer", 2}}},
                                                             {"load_op", "load"},
                                                             {"store_op", "dont_care"}}}));
    CHECK(pass.at("output").at("depth") == att("atlas"));
    CHECK(pass.at("input") == Json::array({"swapchain_like", "atlas"}));
}

TEST_CASE("RC-3b B1 short forms in the root, a feature and a provider's response resolve as the normal form",
          "[rc3b][b1]") {
    for (const auto shorthand : {Shorthand::none, Shorthand::root, Shorthand::feature, Shorthand::provider}) {
        CAPTURE(static_cast<int>(shorthand));
        B1Project project{shorthand};
        const auto variant = project.compile();
        requireB1Resolution(variant);
    }
}

TEST_CASE("RC-3b B1 control: without the normalization the same short forms are refused by name",
          "[rc3b][b1][control]") {
    const auto refused = [](Shorthand shorthand) {
        B1Project project{shorthand};
        const ScopedRenderingJsonNormalizationBypass bypass;
        return refusalOf([&] { (void)project.compile(); });
    };
    // The normal form still compiles with the bypass: the refusals below are
    // of the short forms only.
    {
        B1Project project{Shorthand::none};
        const ScopedRenderingJsonNormalizationBypass bypass;
        requireB1Resolution(project.compile());
    }
    auto refusal = refused(Shorthand::root);
    CHECK(refusal.code == std::string{renderJsonNotNormalizedCode});
    CHECK(refusal.detail == Json{{"path", "present/input"}});
    refusal = refused(Shorthand::feature);
    CHECK(refusal.code == std::string{renderJsonNotNormalizedCode});
    CHECK(refusal.detail == Json{{"path", "to_atlas/input"}});
    // The provider's response meets the check before the core reads it.
    refusal = refused(Shorthand::provider);
    CHECK(refusal.code == std::string{renderJsonNotNormalizedCode});
    CHECK(refusal.detail.at("path").get<std::string>().starts_with("/rendering_passes/0/passes/"));
}

TEST_CASE("RC-3b B1 a bad element among good ones is refused at the same entrances, with its position",
          "[rc3b][b1][refusal]") {
    {
        // Root: present's after ["draw", 7]; output.color ["swapchain", 7].
        B1Project good{Shorthand::root};
        requireB1Resolution(good.compile());
        B1Project after{Shorthand::root, Json{{"after", Json::array({"draw", 7})}}};
        auto refusal = refusalOf([&] { (void)after.compile(); });
        CHECK(refusal.code == std::string{renderJsonShapeInvalidCode});
        CHECK(refusal.detail.at("path") == "/rendering/rendering_passes/0/passes/1/after/1");
        B1Project color{Shorthand::root, Json{{"output", {{"color", Json::array({"swapchain", 7})}, {"depth", nullptr}}}}};
        refusal = refusalOf([&] { (void)color.compile(); });
        CHECK(refusal.code == std::string{renderJsonShapeInvalidCode});
        CHECK(refusal.detail.at("path") == "/rendering/rendering_passes/0/passes/1/output/color/1");
    }
    {
        // A provider's response: provider_pass's after ["from_atlas", 7].
        B1Project provider{Shorthand::provider};
        provider.state.provider_after = Json::array({"from_atlas", 7});
        const auto refusal = refusalOf([&] { (void)provider.compile(); });
        CHECK(refusal.code == std::string{renderJsonShapeInvalidCode});
        CHECK(refusal.detail.at("path").get<std::string>().ends_with("/after/1"));
    }
}

TEST_CASE("RC-3b HDR retargets the scene terminal's attachment and keeps its ops",
          "[rc3b][b1][hdr]") {
    const auto root = TestSupport::renderGraphRoot(Json{
        {"render_targets", Json::array({image("scene")})},
        {"features", Json::array({"engine://features/hdr.json"})},
        {"rendering_passes",
         Json::array({Json{{"name", "main"},
                           {"passes", Json::array({Json{{"name", "draw"},
                                                        {"type", "fullscreen"},
                                                        {"output", {{"color", "scene"}, {"depth", nullptr}}}},
                                                   Json{{"name", "present"},
                                                        {"type", "fullscreen"},
                                                        {"input", "scene"},
                                                        {"output",
                                                         {{"color", Json{{"target", "swapchain"}, {"load_op", "load"}}},
                                                          {"depth", nullptr}}}}})}}})},
    });
    const auto composed = composeRenderFeatureConfig(
        root, RenderFeatureComposeDependencies{
                  .load_feature_json = loadEngineDocument,
                  .runtime_shader_compiler_enabled = true});
    const auto &passes = composed.config.at("rendering_passes").at(0).at("passes");
    const auto byName = [&](const std::string &name) -> const Json & {
        const auto found = std::find_if(passes.begin(), passes.end(),
                                        [&](const Json &pass) { return pass.value("name", "") == name; });
        REQUIRE(found != passes.end());
        return *found;
    };
    // The scene terminal writes the HDR intermediate, its load_op kept; the
    // feature's swapchain output is display; output_transform presents.
    CHECK(byName("present").at("output").at("color") ==
          Json::array({Json{{"target", "scene_ldr_in"}, {"load_op", "load"}}}));
    CHECK(byName("hdr_tonemap").at("output").at("color") == Json::array({att("display")}));
    CHECK(byName("hdr_tonemap").at("input") == Json::array({"scene_ldr_in"}));
    CHECK(byName("output_transform").at("output").at("color") == Json::array({att("swapchain")}));
    REQUIRE_NOTHROW(requireNormalizedRenderingJson(composed.config));
    const ScopedRenderingJsonNormalizationBypass bypass;
    const auto refusal = refusalOf([&] {
        (void)composeRenderFeatureConfig(
            root, RenderFeatureComposeDependencies{
                      .load_feature_json = loadEngineDocument,
                      .runtime_shader_compiler_enabled = true});
    });
    CHECK(refusal.code == std::string{renderJsonNotNormalizedCode});
}

TEST_CASE("RC-3b a feature's own string buffer is its own for the composition and for the feature list",
          "[rc3b][feature-list]") {
    const Json feature{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "scratchy"},
        {"buffers", Json::array({"scratch"})},
        {"passes",
         Json::array({Json{{"insert", "end"},
                           {"pass", {{"name", "use_scratch"},
                                     {"type", "fullscreen"},
                                     {"input", Json::array({"scratch", "external"})},
                                     {"output", {{"color", "swapchain"}, {"depth", nullptr}}}}}}})},
    };
    // The list the Studio shows (renderconfigeditor's listRenderFeatures
    // calls this on the document it read).
    const auto listed = renderFeatureRequiredTargets(feature, true);
    std::vector<std::string> names;
    for (const auto &entry : listed) names.push_back(entry.at("name").get<std::string>());
    CHECK(names == std::vector<std::string>{"external"});
    // The composition owns the same buffer.
    const auto root = TestSupport::renderGraphRoot(Json{
        {"render_targets", Json::array({image("external")})},
        {"features", Json::array({"project://scratchy.json"})},
        {"rendering_passes",
         Json::array({Json{{"name", "main"},
                           {"passes", Json::array({Json{{"name", "draw"},
                                                        {"type", "fullscreen"},
                                                        {"output", {{"color", "external"}, {"depth", nullptr}}}}})}}})},
    });
    const auto composed = composeRenderFeatureConfig(
        root, RenderFeatureComposeDependencies{.load_feature_json = [&](std::string_view) { return feature.dump(); },
                                               .runtime_shader_compiler_enabled = true});
    std::vector<std::string> owned;
    for (const auto &resource : composed.resource_provenance)
        if (resource.source == RenderPipelineProvenanceSource::feature) owned.push_back(resource.name);
    CHECK(owned == std::vector<std::string>{"scratch"});
    CHECK(composed.config.at("buffers") == Json::array({Json{{"name", "scratch"}}}));
}

TEST_CASE("RC-3b preview redirects the swapchain output, not a define of the same spelling",
          "[rc3b][preview]") {
    const auto policy = compileGraphVariantPolicy(GraphVariantPolicyRequest{RenderPipelineGraphVariant::preview});
    auto config = normalizeRenderingJson(Json{
        {"rendering_passes",
         Json::array({Json{{"name", "main"},
                           {"passes", Json::array({Json{{"name", "present"},
                                                        {"type", "fullscreen"},
                                                        {"shader", {{"defines", Json::array({"swapchain"})}}},
                                                        {"output",
                                                         {{"color", Json{{"target", "swapchain"}, {"load_op", "load"}}},
                                                          {"depth", nullptr}}}}})}}})}});
    transformGraphVariantConfig(policy, config);
    const auto &present = config.at("rendering_passes").at(0).at("passes").at(0);
    CHECK(present.at("output").at("color") ==
          Json::array({Json{{"target", "preview_capture"}, {"load_op", "load"}}}));
    CHECK(present.at("shader").at("defines") == Json::array({"swapchain"}));
}

TEST_CASE("RC-3b B3 the generators write the normal form", "[rc3b][b3]") {
    // The composition's own passes (canonical anchors, output_transform,
    // snapshots) and the graph lowering: compose checks its result
    // (requireNormalizedRenderingJson) before returning it.
    const auto root = TestSupport::renderGraphRoot(Json{
        {"render_targets", Json::array({image("scene")})},
        {"snapshots", Json::array({Json{{"name", "opaque"}, {"after", "post_ldr"}}})},
        {"rendering_passes",
         Json::array({Json{{"name", "main"},
                           {"passes", Json::array({Json{{"name", "draw"},
                                                        {"type", "fullscreen"},
                                                        {"output", {{"color", "swapchain"}, {"depth", nullptr}}}}})}}})},
    });
    auto composed = composeRenderFeatureConfig(root);
    REQUIRE_NOTHROW(requireNormalizedRenderingJson(composed.config));
    // ImGui's pass (never in headless / RPC / golden / XR runs, so checked
    // here on its own).
    appendImGuiPassToCanonicalGraphs(composed.config);
    REQUIRE_NOTHROW(requireNormalizedRenderingJson(composed.config));
    const auto &passes = composed.config.at("rendering_passes").at(0).at("passes");
    CHECK(std::any_of(passes.begin(), passes.end(),
                      [](const Json &pass) { return pass.value("name", "") == "imgui_pass"; }));
    // The authoring lowering's result (the root a Studio candidate and the
    // registration compose from).
    const auto lowered = lowerAuthoringDocument(root);
    REQUIRE_NOTHROW(requireNormalizedRenderingJson(lowered.document));
    // The new project templates.
    for (const auto kind : projectTemplates()) {
        CAPTURE(std::string{projectTemplateName(kind)});
        const auto directory = std::filesystem::temp_directory_path() /
                               ("pelican-rc3b-template-" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto created = initializeProjectTemplate(directory, kind);
        const auto project = Json::parse(std::ifstream{created.project_root / "project.json"});
        const auto rendering = project.at("basic_config").at("rendering_config_json").get<std::string>();
        const auto document = Json::parse(std::ifstream{created.project_root / rendering});
        CHECK_NOTHROW(requireNormalizedRenderingJson(document));
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
}

TEST_CASE("RC-3b B2 the bundled documents normalize idempotently and plan the same frame",
          "[rc3b][b2]") {
    const std::filesystem::path source{PELICAN_TEST_SOURCE_DIR};
    std::vector<std::filesystem::path> documents;
    for (const auto *project : {"example", "animgraph_demo", "sprite_demo", "vrm_xr_demo"}) {
        const auto project_json = Json::parse(std::ifstream{source / "projects" / project / "project.json"});
        documents.push_back(source / "projects" / project /
                            project_json.at("basic_config").at("rendering_config_json").get<std::string>());
    }
    for (const auto *directory : {"features", "render_pipelines"})
        for (const auto &entry : std::filesystem::directory_iterator{source / "src" / "core" / "resources" / directory})
            if (entry.path().extension() == ".json") documents.push_back(entry.path());
    REQUIRE(documents.size() >= 4 + 21 + 1);
    std::size_t planned = 0;
    for (const auto &path : documents) {
        CAPTURE(path.string());
        const auto authored = Json::parse(std::ifstream{path});
        const auto normalized = normalizeRenderingJson(authored);
        CHECK(normalizeRenderingJson(normalized) == normalized);
        REQUIRE_NOTHROW(requireNormalizedRenderingJson(normalized));
        // A root's families plan the same frame from either form (the core
        // parser reads both until RC-3c).
        if (authored.value("schema", "") != "pelican.graph") continue;
        const auto &section = authored.at("rendering");
        if (!section.contains("rendering_passes")) continue;
        auto plans = [](const Json &config) {
            auto encoded = Json::array();
            for (const auto &graph : parseFrameGraphDefinitionsFromConfigJson(config))
                encoded.push_back(framePlanToJson(planFrameGraph(graph), nullptr));
            return encoded;
        };
        Json before;
        try {
            before = plans(section);
        } catch (const std::exception &) {
            continue; // a root whose passes plan only once composed
        }
        CHECK(plans(normalized.at("rendering")) == before);
        ++planned;
    }
    // example, animgraph_demo and sprite_demo plan before composition.
    CHECK(planned >= 3);
}

} // namespace Pelican
