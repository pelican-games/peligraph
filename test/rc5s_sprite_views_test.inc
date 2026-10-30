void GoldenHarness::runRc5sSpriteViews() {
#if PELICAN_WITH_OPENXR
    setupLogger();
    requireGoldenVulkanDevice();
    const auto cases = loadGoldenInventoryCases();
    auto found = std::find_if(cases.begin(), cases.end(), [](const auto &c) { return c.mode == "sprite_ortho_atlas"; });
    REQUIRE(found != cases.end());
    auto fixture = *found;
    fixture.width = fixture.height = 64;
    for (const auto samples : {1u, 4u}) for (const bool probe : {false, true}) {
        CAPTURE(samples, probe);
        FastModuleContainer modules;
        const auto root = makeTempProjectDir("rc5s_sprite_stereo");
        const auto project = writeSpriteProject(root, fixture, true);
        RgbaImage red{32, 32, std::vector<std::uint8_t>(32 * 32 * 4)};
        for (std::size_t i = 0; i < red.pixels.size(); i += 4) {
            red.pixels[i] = red.pixels[i + 3] = 255;
        }
        writePng(root / "assets/page0.png", red);
        writeTextFile(root / "shaders/occlusion.frag", R"glsl(#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_frame.glsl"
layout(location=0) out vec4 color;
void main() {
    bool blocked = pelicanFrame.view_index == 0u ? gl_FragCoord.x < 20.0 : gl_FragCoord.x >= 44.0;
    gl_FragDepth = blocked ? 0.25 : 1.0;
    color = vec4(0,1,0,1);
})glsl");
        writeTextFile(root / "shaders/depth_probe.frag", R"glsl(#version 450
layout(location=0) out vec4 color;
void main() { gl_FragDepth = 0.75; color = vec4(0,0,1,1); }
)glsl");
        auto passes = nlohmann::json::array({{
            {"name", "lighting_pass"}, {"type", "raster"}, {"draw", {{"vertex_count", 3}}},
            {"output", {{"color", "sprite_color"}, {"depth", "sprite_depth"}}},
            {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "project://shaders/occlusion"}}},
            {"raster_state", {{"depth_test", true}, {"depth_write", true}, {"depth_compare", "always"}}}
        }});
        if (probe) passes.push_back({
            {"name", "depth_probe"}, {"type", "raster"}, {"draw", {{"vertex_count", 3}}}, {"after", {"sprite_draw"}},
            {"output", {{"color", "sprite_color"}, {"depth", "sprite_depth"}}},
            {"color_load_op", "load"}, {"depth_load_op", "load"},
            {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "project://shaders/depth_probe"}}},
            {"raster_state", {{"depth_test", true}, {"depth_write", false}, {"depth_compare", "less"}}}
        });
        passes.push_back({{"name", "present"}, {"type", "fullscreen"}, {"input", {"sprite_color"}},
            {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
            {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}});
        nlohmann::json rendering{
            {"features", nlohmann::json::array({
                {{"ref", "project://features/sprite.json"}, {"parameters", {{"color", "sprite_color"}, {"depth", "sprite_depth"}}}},
                "engine://features/gpu_timing.json"})},
            {"xr", {{"view_execution", "sequential"}}},
            {"multisampling", {{"samples", samples}, {"fallback", "error"}, {"scope", "all"}}},
            {"render_targets", nlohmann::json::array({
                {{"name", "sprite_color"}, {"format", "R8G8B8A8_UNORM"}, {"format_class", "scene"}, {"role", "color"},
                 {"extent_scale", 1.0}, {"layers", 2}, {"usage", {"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"}}},
                {{"name", "sprite_depth"}, {"format", "D32_SFLOAT"}, {"extent_scale", 1.0}, {"layers", 2},
                 {"usage", {"DEPTH_STENCIL_ATTACHMENT", "TRANSFER_SRC"}}}})},
            {"rendering_passes", nlohmann::json::array({{{"name", "main"}, {"passes", passes}}})}
        };
        auto observed_root = TestSupport::renderGraphRoot(rendering);
        TestSupport::retainObserved(observed_root, "main", {"sprite_depth", "sprite_color"});
        writeTextFile(root / "passes/main.json", observed_root.dump(2));
        GET_MODULE(PathResolver).setup(root, false);
        GET_MODULE(ProjectSource).setProjectData(project.dump());
        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.shader_hot_reload = false;
        launch.headless_extent = vk::Extent2D{64, 64};
        (void)GET_MODULE(VulkanManageCore);
        launch.xr_active = true;
        GET_MODULE(ECSPredefinedRegistration).reg();
        test_support::loadScene(GET_MODULE(SceneLoader), "default_scene");
        GET_MODULE(ECSCore).update();
        auto &renderer = GET_MODULE(Renderer);
        renderer.selectGraphVariant(RenderGraphVariant::xr);
        RendererTestAccess::setExecutionTracing(renderer, true);
        auto &targets = GET_MODULE(RenderTargetContainer);
        const auto display = targets.getRenderTargetIdByName("sprite_color");
        const auto depth = targets.getRenderTargetIdByName("sprite_depth");
        CHECK(targets.getMetadata(display).array_layers == 2);
        CHECK(targets.getMetadata(depth).array_layers == 2);
        CHECK(targets.getMetadata(display).samples == samples);
        CHECK(targets.getMetadata(depth).samples == samples);
        CHECK(targets.hasSeparateAttachment(display) == (samples > 1));
        CHECK(targets.hasSeparateAttachment(depth) == (samples > 1));
        RenderViewFamily family{.family_id = std::string{mainRenderViewFamilyId}, .views = {RenderViewParameters{}, RenderViewParameters{}}};
        family.views[0].view[3][0] = -0.25f;
        family.views[1].view[3][0] = 0.25f;
        family.views[0].view_id = "$xr/0";
        family.views[1].view_id = "$xr/1";
        Test::VulkanSyntheticStereoTarget stereo{launch.headless_extent, GET_MODULE(RenderTarget).getSwapchainFormat()};
        std::array<std::vector<std::uint8_t>, 2> empty_images;
        std::vector<std::uint8_t> empty_depth;
        for (const bool draw : {false, true}) {
            auto &scene = GET_MODULE(SpriteScene);
            scene.clear();
            if (draw) {
                TransformComponent transform{{0,0,0.5f}, {1,0,0,0}, {1,1,1}};
                SpriteViewComponent sprite;
                sprite.texture = "atlas#sprite/page0";
                sprite.size = {1,1}; sprite.has_explicit_size = 1;
                const std::array items{SpriteSceneItem{EntityId{123,1}, &transform, &sprite}};
                scene.prepareAssets(items); scene.rebuild(items);
            }
            renderer.renderLogicalFrame(stereo, family);
            GET_MODULE(VulkanManageCore).waitIdle();
            const auto snapshots = RendererTestAccess::lastViewSnapshots(renderer);
            REQUIRE(snapshots.size() == 2);
            for (std::size_t eye = 0; eye < 2; ++eye) {
                CHECK(snapshots[eye].view == family.views[eye].view);
                CHECK(snapshots[eye].projection_non_jittered == family.views[eye].projection);
                const auto image = stereo.readback(static_cast<std::uint32_t>(eye));
                REQUIRE(image.size() == 64 * 64 * 4);
                if (!draw) empty_images[eye] = image;
                else if (probe) CHECK(image == empty_images[eye]);
                // Exact interior/edge pixel oracle: different view translations
                // and occluder masks exercise both layer selection and FrameUBO.
                std::size_t wrong = 0;
                for (std::size_t y = 0; y < 64; ++y) for (std::size_t x = 0; x < 64; ++x) {
                    const bool blocked = eye == 0 ? x < 20 : x >= 44;
                    const bool inside = x >= (eye == 0 ? 8 : 24) && x < (eye == 0 ? 40 : 56) && y >= 16 && y < 48;
                    const auto channel = probe && !blocked ? 2u : draw && inside && !blocked ? 0u : 1u;
                    for (std::size_t c = 0; c < 4; ++c)
                        if (image[(y * 64 + x) * 4 + c] != (c == channel || c == 3 ? 255 : 0)) ++wrong;
                }
                CAPTURE(eye, draw, wrong);
                CHECK(wrong == 0);
            }
            const auto depth_bytes = RendererTestAccess::readRenderTarget(renderer, "sprite_depth", {.layer_count = 2}).bytes;
            if (!draw) empty_depth = depth_bytes;
            else CHECK(depth_bytes == empty_depth);
            const auto left = RendererTestAccess::readRenderTarget(renderer, "sprite_color", {}).bytes;
            const auto right = RendererTestAccess::readRenderTarget(renderer, "sprite_color", {.base_array_layer = 1}).bytes;
            if (draw || probe) CHECK(left != right);
        }
        std::filesystem::remove_all(root);
    }
#else
    SKIP("OpenXR is disabled; stereo sprite acceptance was not executed");
#endif
}
