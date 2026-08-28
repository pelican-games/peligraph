#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderingpass/passimplementationregistry.hpp"
#include "../src/core/userpublic/details/reload/registrationowner.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/devstudio/model/frameplanmodel.hpp"
#include "../src/devstudio/view/frameplanwidget.hpp"
#include "../src/devstudio/viewport/embeddedviewport.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QByteArray>
#include <QPushButton>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Pelican {
namespace {

QApplication &application() {
    if (auto *existing = qobject_cast<QApplication *>(
            QCoreApplication::instance())) {
        return *existing;
    }
    static int argc = 1;
    static char name[] = "pelican-wp354-production-test";
    static char *argv[] = {name, nullptr};
    static auto instance =
        std::make_unique<QApplication>(argc, argv);
    return *instance;
}

void writeText(const std::filesystem::path &path,
               std::string_view text) {
    std::ofstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error(
            "failed to create WP354 production fixture: " +
            path.generic_string());
    }
    stream.write(text.data(),
                 static_cast<std::streamsize>(text.size()));
}

struct ProviderState {
    std::uint32_t calls = 0;
    std::string implementation =
        "fixture.wp354.fullscreen@1";
    std::string vertex =
        "shaders/effective_vertex";
    std::string fragment =
        "shaders/effective_fragment";
};

RenderPass::Status resolveFullscreen(
    void *context,
    const RenderPass::ResolveFullscreenInputV1 *input,
    RenderPass::FullscreenImplementationV1 *output) noexcept {
    if (context == nullptr || input == nullptr ||
        output == nullptr ||
        input->struct_size <
            sizeof(RenderPass::ResolveFullscreenInputV1) ||
        input->version != RenderPass::descriptorVersionV1 ||
        input->authored_implementation == nullptr) {
        return RenderPass::Status::invalid_argument;
    }
    auto &state = *static_cast<ProviderState *>(context);
    ++state.calls;
    *output = RenderPass::descriptor<
        RenderPass::FullscreenImplementationV1>();
    output->implementation_id_utf8 =
        state.implementation.data();
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            state.implementation.size());
    output->vertex_shader_utf8 = state.vertex.data();
    output->vertex_shader_size =
        static_cast<std::uint32_t>(state.vertex.size());
    output->fragment_shader_utf8 = state.fragment.data();
    output->fragment_shader_size =
        static_cast<std::uint32_t>(state.fragment.size());
    return RenderPass::Status::ok;
}

RenderPass::ProviderV1 provider(ProviderState &state) {
    static constexpr std::string_view name =
        "fixture.wp354.production";
    auto result =
        RenderPass::descriptor<RenderPass::ProviderV1>();
    result.capability_bits =
        RenderPass::builtinProviderCapabilitiesV1;
    result.name_utf8 = name.data();
    result.name_size =
        static_cast<std::uint32_t>(name.size());
    result.context = &state;
    result.resolve_fullscreen = resolveFullscreen;
    return result;
}

class ScopedProvider final {
    internal::RegistrationOwner owner_ =
        internal::allocateRegistrationOwner();
    RenderPass::ProviderHandleV1 handle_{};
    bool registered_ = false;

  public:
    explicit ScopedProvider(ProviderState &state) {
        registered_ =
            passImplementationRegistry().registerProvider(
                provider(state), owner_, handle_) ==
            RenderPass::Status::ok;
        if (registered_) {
            passImplementationRegistry().activateOwner(owner_);
        }
    }

    ~ScopedProvider() {
        if (registered_) {
            (void)passImplementationRegistry()
                .unregisterProvider(handle_, owner_);
            passImplementationRegistry().releaseOwner(owner_);
        }
        internal::releaseRegistrationOwner(owner_);
    }

    [[nodiscard]] bool registered() const noexcept {
        return registered_;
    }
};

const nlohmann::json *nodeNamed(
    const nlohmann::json &plan, std::string_view name) {
    const auto found = std::find_if(
        plan.at("nodes").begin(), plan.at("nodes").end(),
        [&](const auto &node) {
            return node.value("name", std::string{}) == name;
        });
    return found == plan.at("nodes").end()
               ? nullptr
               : std::addressof(*found);
}

} // namespace

TEST_CASE(
    "WP354 production config and provider flow through renderer model and Open action",
    "[wp354][production][headless][shader-resolution]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    (void)application();
    setupLogger();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = std::filesystem::path{
        temporary.path().toStdWString()};

    try {
        writeText(
            root / "scene.json",
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeText(
            root / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
        std::filesystem::create_directories(root / "shaders");
        writeText(
            root / "shaders" / "effective_vertex.vert",
            R"glsl(#version 450
const vec2 positions[3] = vec2[3](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));
void main() { gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0); }
)glsl");
        writeText(
            root / "shaders" / "effective_fragment.frag",
            R"glsl(#version 450
layout(location=0) out vec4 out_color;
void main() { out_color = vec4(0.25, 0.5, 0.75, 1.0); }
)glsl");

        const auto pipeline = nlohmann::json::parse(R"json({
          "render_targets": [
            {"name":"wp354_authored_color","extent_scale":1.0,
             "format":"R8G8B8A8_UNORM","format_class":"data",
             "usage":["COLOR_ATTACHMENT","SAMPLED","TRANSFER_SRC"]},
            {"name":"wp354_snapshot_color","extent_scale":1.0,
             "format":"R8G8B8A8_UNORM","format_class":"data",
             "usage":["TRANSFER_DST","SAMPLED"]},
            {"name":"wp354_material_color","extent_scale":1.0,
             "format":"R16G16B16A16_SFLOAT",
             "format_class":"explicit(R16G16B16A16_SFLOAT)",
             "usage":["COLOR_ATTACHMENT","SAMPLED"]},
            {"name":"wp354_material_depth","extent_scale":1.0,
             "format":"D32_SFLOAT","format_class":"data",
             "usage":["DEPTH_STENCIL_ATTACHMENT"]},
            {"name":"wp354_shadow_depth","extent_scale":1.0,
             "format":"D32_SFLOAT","format_class":"data",
             "usage":["DEPTH_STENCIL_ATTACHMENT","SAMPLED"]}
          ],
          "rendering_passes": [{
            "name":"wp354_production_main",
            "passes":[
              {"name":"wp354_authored","type":"fullscreen",
               "output":{"color":"wp354_authored_color","depth":null},
               "shader":{"vertex":"shaders/effective_vertex",
                         "fragment":"shaders/effective_fragment"}},
              {"name":"wp354_snapshot","type":"snapshot_copy",
               "source":"wp354_authored_color",
               "destination":"wp354_snapshot_color",
               "snapshot":"wp354_snapshot_color",
               "snapshot_after":"wp354_authored"},
              {"name":"wp354_shadow","type":"shadow_depth",
               "output":{"color":null,"depth":"wp354_shadow_depth"},
               "depth_store_op":"store"},
              {"name":"wp354_material","type":"material",
               "material_contract":"forward_opaque_v1",
               "output":{"color":"wp354_material_color",
                         "depth":"wp354_material_depth"},
               "depth_store_op":"store"},
              {"name":"wp354_provided","type":"fullscreen",
               "after":["wp354_material","wp354_shadow"],
               "implementation":{"provider":"fixture.wp354.production"},
               "output":{"color":"swapchain","depth":null},
               "shader":{"vertex":"shaders/declared",
                         "fragment":"shaders/declared"}}
            ]
          }]
        })json");
        writeText(root / "pipeline.json", pipeline.dump(2));

        nlohmann::json project{
            {"schema", "pelican.project"},
            {"version", 1},
            {"name", "wp354-production"},
            {"basic_config",
             {{"window_size", {{"width", 16}, {"height", 16}}},
              {"scene_data_json", "scene.json"},
              {"asset_data_json", "assets.json"},
              {"default_scene_id", "default_scene"},
              {"rendering_config_json", "pipeline.json"},
              {"default_rendering_pass",
               "wp354_production_main"}}},
        };
        writeText(root / "project.json", project.dump(2));

        ProviderState provider_state;
        ScopedProvider scoped_provider{provider_state};
        REQUIRE(scoped_provider.registered());

        FastModuleContainer modules;
        GET_MODULE(ProjectSource).setProjectData(project.dump());
        GET_MODULE(PathResolver).setup(
            root, false, project.dump());
        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent = vk::Extent2D{16, 16};
        launch.headless_frames = 1;
        GET_MODULE(EngineTime).setup(
            EngineTime::Mode::fixed_step, 1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        REQUIRE(provider_state.calls == 1);
        const auto plan = renderer.currentFramePlanJson();
        REQUIRE(plan.at("profile") == "runtime");
        const auto *wire_node =
            nodeNamed(plan, "wp354_provided");
        REQUIRE(wire_node != nullptr);
        const auto &wire_stages = wire_node->at(
            "shader_resolution").at("stages");
        REQUIRE(wire_stages.size() == 2);
        REQUIRE(wire_stages[0].at("stage") == "vertex");
        REQUIRE(wire_stages[0].at("declared_ref") ==
                "shaders/declared");
        REQUIRE(wire_stages[0].at("effective_ref") ==
                "shaders/effective_vertex");
        REQUIRE(wire_stages[0].at("origin") == "provider");
        REQUIRE(wire_stages[0].at("source_open_ref") ==
                "project://shaders/effective_vertex.vert");
        REQUIRE(wire_stages[1].at("stage") == "fragment");
        REQUIRE(wire_stages[1].at("declared_ref") ==
                "shaders/declared");
        REQUIRE(wire_stages[1].at("effective_ref") ==
                "shaders/effective_fragment");
        REQUIRE(wire_stages[1].at("origin") == "provider");
        REQUIRE(wire_stages[1].at("source_open_ref") ==
                "project://shaders/effective_fragment.frag");

        const auto *authored = nodeNamed(plan, "wp354_authored");
        REQUIRE(authored != nullptr);
        const auto &authored_stages = authored->at(
            "shader_resolution").at("stages");
        REQUIRE(authored_stages[0].at("origin") == "authored");
        REQUIRE(authored_stages[0].at("declared_ref") ==
                "shaders/effective_vertex");
        REQUIRE(authored_stages[0].at("effective_ref") ==
                "shaders/effective_vertex");

        const auto *shadow = nodeNamed(plan, "wp354_shadow");
        REQUIRE(shadow != nullptr);
        const auto &shadow_stage = shadow->at(
            "shader_resolution").at("stages").at(0);
        REQUIRE(shadow_stage.at("stage") == "vertex");
        REQUIRE_FALSE(shadow_stage.contains("declared_ref"));
        REQUIRE(shadow_stage.at("effective_ref") ==
                "engine://shadow_depth");
        REQUIRE(shadow_stage.at("origin") == "engine_default");

        const auto *material = nodeNamed(plan, "wp354_material");
        REQUIRE(material != nullptr);
        REQUIRE(material->at("shader_resolution") ==
                nlohmann::json{{"state", "material_owned"}});
        const auto *snapshot = nodeNamed(plan, "wp354_snapshot");
        REQUIRE(snapshot != nullptr);
        REQUIRE(snapshot->at("shader_resolution") ==
                nlohmann::json{{"state", "not_applicable"}});

        const auto model =
            PelicanStudio::buildFramePlanModel(plan.dump());
        const auto modeled = std::find_if(
            model.nodes.begin(), model.nodes.end(),
            [](const auto &node) {
                return node.name == "wp354_provided";
            });
        REQUIRE(modeled != model.nodes.end());
        REQUIRE(modeled->shader_resolution.resolved());
        REQUIRE(modeled->shader_resolution.stages.size() == 2);
        REQUIRE(modeled->shader_resolution.stages[0].origin ==
                "provider");

        PelicanStudio::EmbeddedViewport viewport;
        PelicanStudio::FramePlanWidget widget{&viewport};
        widget.setProjectRoot(root);
        std::optional<std::filesystem::path> opened;
        widget.setShaderSourceOpenAction(
            [&](const std::filesystem::path &path) {
                opened = path;
                return true;
            });
        widget.receiveResult(
            QByteArray::fromStdString(plan.dump()));
        QApplication::processEvents();
        const auto buttons =
            widget.findChildren<QPushButton *>(
                QStringLiteral("pelican.shaderSourceOpen"));
        const auto open = std::find_if(
            buttons.begin(), buttons.end(),
            [](const auto *button) {
                return button->property("pelicanNode") ==
                           QStringLiteral("wp354_provided") &&
                       button->property("pelicanStage") ==
                           QStringLiteral("vertex");
            });
        REQUIRE(static_cast<bool>(open != buttons.end()));
        REQUIRE((*open)->isEnabled());
        (*open)->click();
        REQUIRE(opened == std::filesystem::canonical(
                              root / "shaders" /
                              "effective_vertex.vert"));

        GET_MODULE(VulkanManageCore).waitIdle();
    } catch (const std::exception &error) {
        TestSupport::skipIfVulkanDeviceUnavailable(
            error,
            "WP354 production Vulkan compilation unavailable");
        throw;
    }
#else
    SUCCEED("compiler-OFF source paths are covered by the shared resolver contract");
#endif
}

} // namespace Pelican
