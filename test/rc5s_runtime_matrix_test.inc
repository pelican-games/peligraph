namespace {
void rc5sCheckRuntime(bool xr, bool lit = false) {
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.shader_hot_reload = false;
    launch.headless_extent = vk::Extent2D{64, 64};
    (void)GET_MODULE(VulkanManageCore);
    launch.xr_active = xr;
    if (lit) GET_MODULE(LightContainer).load({LightLoadEntry{
        .name = "Sun", .component = LightCodecData{
            .direction = {0.45f, -0.35f, -1.0f}, .intensity = 4.0f}}});
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    auto &renderer = GET_MODULE(Renderer);
    RendererTestAccess::setExecutionTracing(renderer, true);
    if (xr) renderer.selectGraphVariant(RenderGraphVariant::xr);
    const auto check_nodes = [](const nlohmann::json &nodes) {
        REQUIRE_FALSE(nodes.empty());
        for (const auto &node : nodes)
            CHECK_FALSE(node.at("name").get<std::string>().starts_with("__anchor_"));
        const auto output = std::find_if(nodes.begin(), nodes.end(), [](const auto &node) {
            return node.at("name") == "output_transform";
        });
        REQUIRE(output != nodes.end());
        // XR mirror copies run outside the family after its terminal.
        for (auto node = std::next(output); node != nodes.end(); ++node)
            CHECK(node->at("name").template get<std::string>().starts_with("xr_mirror"));
    };
    time.advance();
    if (xr) {
        Test::VulkanSyntheticStereoTarget output{
            launch.headless_extent, GET_MODULE(RenderTarget).getSwapchainFormat()};
        RenderViewFamily views{.family_id = std::string{mainRenderViewFamilyId},
                               .views = {RenderViewParameters{}, RenderViewParameters{}}};
        views.views[0].view_id = "$xr/0";
        views.views[1].view_id = "$xr/1";
        renderer.renderLogicalFrame(output, views);
    } else {
        renderer.render();
    }
    GET_MODULE(VulkanManageCore).waitIdle();
    const auto trace = RendererTestAccess::lastExecutionTrace(renderer);
    if (xr) {
        REQUIRE(trace.at("views").size() == 2);
        for (const auto &view : trace.at("views")) check_nodes(view.at("nodes"));
        if (lit) {
            const auto &first = trace.at("views")[0].at("nodes");
            const auto shadow = std::find_if(first.begin(), first.end(), [](const auto &node) {
                return node.at("name") == "shadow_depth";
            });
            REQUIRE(shadow != first.end());
            CHECK(shadow->value("recorded", true));
            const auto &second = trace.at("views")[1].at("nodes");
            CHECK(std::none_of(second.begin(), second.end(), [](const auto &node) {
                return node.at("name") == "shadow_depth";
            }));
        }
    } else {
        check_nodes(trace.at("nodes"));
    }
    auto &timing = GET_MODULE(RenderTiming);
    timing.flush();
    const auto status = timing.statusJson();
    REQUIRE(status.at("enabled").get<bool>());
    REQUIRE_FALSE(status.at("nodes").empty());
    for (const auto &row : status.at("nodes"))
        CHECK_FALSE(row.at("node_name").get<std::string>().starts_with("__anchor_"));
    CHECK(timing.allGpuQueriesCollectedForTesting());
}
} // namespace

void GoldenHarness::runRc5sRuntimeMatrix() {
    setupLogger();
    requireGoldenVulkanDevice();
    std::size_t shipping = 0;
    for (const auto *name : {"animgraph_demo", "example", "pathtrace_demo", "posegraph_demo",
                             "raytracing_demo", "sprite_demo", "vrm_xr_demo"}) {
        CAPTURE(name);
        FastModuleContainer modules;
        const auto root = sourceRoot() / "projects" / name;
        GET_MODULE(PathResolver).setup(root, false);
        GET_MODULE(ProjectSource).setProjectData(readTextFile(root / "project.json"));
        auto &config = GET_MODULE(ProjectBasicConfig);
        auto document = nlohmann::json::parse(config.renderingConfigJson());
        auto &features = renderingSectionOf(document)["features"];
        if (features.is_null()) features = nlohmann::json::array();
        features.push_back("engine://features/gpu_timing.json");
        config.publishRenderingConfigJson(document.dump());
        // The shipping render graphs run with empty ECS content here; A3 runs
        // the actual project scenes and their catalog game logic separately.
        rc5sCheckRuntime(false);
        ++shipping;
    }
    CHECK(shipping == 7);
    std::size_t recipes = 0;
#if !PELICAN_WITH_OPENXR
    FAIL("RC-5s runtime matrix requires OpenXR enabled for all sixteen configurations");
#else
    for (const bool lit : {false, true}) for (const bool hdr : {false, true})
    for (const bool extras : {false, true}) for (const bool xr : {false, true}) {
        CAPTURE(lit, hdr, extras, xr);
        const auto root = makeTempProjectDir("rc5s_runtime_recipe");
        {
            FastModuleContainer modules;
            auto document = TestSupport::renderGraphRoot(nlohmann::json{
                {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                {"xr", {{"view_execution", "sequential"}}},
                {"features", nlohmann::json::array({"engine://features/gpu_timing.json"})}});
            auto &features = document["rendering"]["features"];
            if (lit) {
                features.push_back("engine://features/shadow_directional.json");
                features.push_back("engine://features/sky_ambient.json");
            }
            if (hdr) features.push_back("engine://features/hdr.json");
            if (extras) {
                auto feature = nlohmann::json::parse(engineResourceOrThrow("features/ui.json"));
                // Keep the built-in runtime name when executing this copied
                // feature; the compiler-only matrix calls the copy local_ui.
                writeTextFile(root / "features/local.json", feature.dump());
                features.push_back("project://features/local.json");
                const nlohmann::json type{{"semantic", "pelican.render.color_signal@1"},
                    {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
                document["nodes"] = nlohmann::json::array({
                    {{"id", "probe_image"}, {"op", "render.image"},
                     {"parameters", {{"type", type}, {"format", "R16G16B16A16_SFLOAT"},
                         {"extent_scale", 1.0}, {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}}, {"outputs", {"resource"}}},
                    {{"id", "probe_fill"}, {"op", "render.fullscreen"},
                     {"parameters", {{"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "project://shaders/probe"}}},
                         {"ports", {{"color", {{"kind", "image"}, {"type", type}, {"access", "write"},
                             {"attachment", "color"}, {"location", 0}}}}}}},
                     {"inputs", {{"color", {{"node", "probe_image"}, {"port", "resource"}}}}}, {"outputs", {"color"}}}});
                writeTextFile(root / "shaders/probe.frag", "#version 450\nlayout(location=0) out vec4 color; void main(){color=vec4(0.0);}\n");
            }
            writeTextFile(root / "passes/main.json", document.dump());
            writeTextFile(root / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
            writeTextFile(root / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[]})");
            writeTextFile(root / "ui/ui.json", R"({"schema":"pelican.ui","version":1,"key":"rc5s","revision":"1","direction":"ltr","root":{"id":"root","type":"panel","children":[]}})");
            auto project = makeShadowProjectJson();
            project["basic_config"]["rendering_config_json"] = "passes/main.json";
            project["basic_config"]["default_rendering_pass"] = "main_render";
            project["basic_config"]["asset_data_json"] = "assets.json";
            project["basic_config"]["ui_config_json"] = "ui/ui.json";
            GET_MODULE(PathResolver).setup(root, false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());
            try {
                rc5sCheckRuntime(xr, lit);
            } catch (const std::exception &error) {
                INFO(GET_MODULE(Renderer).currentFramePlanJson().dump(2));
                FAIL(error.what());
            }
        }
        std::filesystem::remove_all(root);
        ++recipes;
    }
#endif
    CHECK(recipes == 16);
}
