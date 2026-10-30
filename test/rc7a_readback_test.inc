namespace {
#if PELICAN_RUNTIME_SHADER_COMPILER
void rc7aObservationControlProject(const std::filesystem::path &directory) {
    writeTextFile(directory / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
    writeTextFile(directory / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[]})");
    using Json = nlohmann::json;
    auto root = TestSupport::renderGraphRoot(Json{
        {"render_targets", Json::array({{{"name", "observed"}, {"format", "R8_UNORM"},
            {"format_class", "explicit(R8_UNORM)"}, {"extent_scale", 1.0},
            {"usage", {"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"}}}})},
        {"rendering_passes", Json::array({{{"name", "main_render"}, {"passes", Json::array({
            {{"name", "write_observed"}, {"type", "fullscreen"},
             {"output", {{"color", "observed"}, {"depth", nullptr}}},
             {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}},
            {{"name", "present"}, {"type", "fullscreen"}, {"input", {"observed"}},
             {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
             {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}}
        })}}})}});
    TestSupport::retainObserved(root, "main_render", {"observed"});
    writeTextFile(directory / "hybrid.json", root.dump(2));
    configureRayQueryTestRuntime(directory);
}
#endif
}

TEST_CASE("RC-7a fix2 unrelated transient image keeps its representation and usage", "[rc7a][rc7a-fix2][gpu][headless]") {
    using Json = nlohmann::json;
    const vk::ApplicationInfo application{"rc7a-fix2", 1, "pelican", 1, VK_API_VERSION_1_3};
    const auto instance = vk::createInstanceUnique(vk::InstanceCreateInfo{{}, &application});
    const auto devices = instance->enumeratePhysicalDevices();
    REQUIRE_FALSE(devices.empty());
    const VulkanRenderCompilerBackendContext backend{vk::Format::eB8G8R8A8Srgb, {64, 64},
        devices.front(), {}, {}, VulkanRenderCompilerDevicePlanningMode::device_required};
    auto config = Json{{"render_targets", Json::array()}, {"rendering_passes", Json::array()}};
    auto passes = Json::array();
    for (const auto *name : {"scratch", "observed"}) {
        auto usage = Json::array({"COLOR_ATTACHMENT"});
        if (std::string_view{name} == "observed") usage.push_back("TRANSFER_SRC");
        config["render_targets"].push_back({{"name", name}, {"format", "R8_UNORM"},
            {"format_class", "explicit(R8_UNORM)"}, {"extent_scale", 1.0}, {"usage", usage}});
        passes.push_back({{"name", std::string{"write_"} + name}, {"type", "fullscreen"},
            {"color_store_op", "dont_care"}, {"output", {{"color", name}, {"depth", nullptr}}},
            {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}});
    }
    passes.push_back({{"name", "present"}, {"type", "fullscreen"},
        {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
        {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}});
    config["rendering_passes"].push_back({{"name", "main_render"}, {"passes", passes}});
    auto root = TestSupport::renderGraphRoot(config);
    PathResolver paths;
    const GraphTransformRegistrySnapshot transforms;
    const RenderStrategyRegistrySnapshot strategies;
    const SubgraphReplacementRegistrySnapshot replacements;
    const std::vector<RenderCompilerProgramVariantRequest> variants{{.graph_variant = RenderPipelineGraphVariant::flat}};
    const auto compile = [&] {
        return runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), {
            .rendering_config = root, .path_resolver = paths, .runtime_shader_compiler_enabled = true,
            .graph_transforms = transforms, .render_strategies = strategies, .subgraph_replacements = replacements,
            .backend_context = backend, .variants = variants});
    };
    const auto resource = [](const auto &result) -> const VulkanPhysicalResourcePlan & {
        const auto &plan = *requireVulkanRenderCompilerPhysicalPackage(*result.variants[0].physical_package).target_plans.at("main_render");
        const auto found = std::find_if(plan.resources.begin(), plan.resources.end(), [](const auto &r) { return r.logical_resource == "scratch"; });
        REQUIRE(found != plan.resources.end());
        return *found;
    };
    const auto automatic = compile();
    REQUIRE(resource(automatic).representation == VulkanResourceRepresentation::transient_attachment);
    const auto *before = automatic.generation_resources.findImage("scratch");
    REQUIRE(before);
    REQUIRE(before->usage == (ImageUsageBit::color_attachment | ImageUsageBit::transient_attachment));
    TestSupport::retainObserved(root, "main_render", {"observed"});
    const auto retained = compile();
    CHECK(resource(retained).representation == resource(automatic).representation);
    const auto *after = retained.generation_resources.findImage("scratch");
    REQUIRE(after);
    CHECK(after->usage == before->usage);
    CHECK(after->declaration.usage == before->declaration.usage);
    const auto &plan = *requireVulkanRenderCompilerPhysicalPackage(*retained.variants[0].physical_package).target_plans.at("main_render");
    CHECK(plan.effective_image_usage.at("scratch") == after->usage);
    CHECK_NOTHROW(validateTargetReadbackRetention(&plan, "observed", plan.graph));
}

// Catch expects the listener's missing-readback CHECK to fail. If successful
// assertions stop reaching the listener, this case unexpectedly passes and is red.
TEST_CASE("RC-7a fix2 observation audit rejects renderer creation without readback", "[rc7a][gpu][headless][!shouldfail]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto directory = makeTempProjectDir();
    FastModuleContainer modules;
    RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
    rc7aObservationControlProject(directory);
    REQUIRE_NOTHROW(GET_MODULE(Renderer));
    std::filesystem::remove_all(directory);
#else
    SKIP("runtime shader compiler unavailable");
#endif
}

TEST_CASE("RC-7a fix2 observation audit preserves an explicit skip", "[.][rc7a][gpu][headless]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto directory = makeTempProjectDir();
    FastModuleContainer modules;
    RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
    rc7aObservationControlProject(directory);
    REQUIRE_NOTHROW(GET_MODULE(Renderer));
    std::filesystem::remove_all(directory);
    SKIP("observation audit skip control");
#else
    SKIP("runtime shader compiler unavailable");
#endif
}

TEST_CASE("RC-7a F1 F2 F5 named readback promises and unobserved neighbor", "[rc7a][gpu][headless]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    FastModuleContainer modules;
    RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
    using Json = nlohmann::json;
    writeTextFile(temp_dir / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
    writeTextFile(temp_dir / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[]})");
    writeTextFile(temp_dir / "shaders/retained.frag", R"(#version 450
layout(location=0) out vec4 color;
void main() { color=vec4(0.25,0.5,0.75,1.0); }
)");
    auto config = Json{{"features", Json::array()},
        {"render_targets", Json::array()}, {"rendering_passes", Json::array()}};
    auto passes = Json::array();
    for (const auto *name : {"observed", "neighbor"}) {
        config["render_targets"].push_back({{"name", name}, {"format", "R8_UNORM"},
            {"format_class", "explicit(R8_UNORM)"}, {"extent_scale", 1.0}, {"usage", {"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"}}});
        passes.push_back({{"name", std::string{"write_"} + name}, {"type", "fullscreen"},
            {"color_store_op", "dont_care"}, {"output", {{"color", name}, {"depth", nullptr}}},
            {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "project://shaders/retained"}}}});
    }
    passes.push_back({{"name", "present"}, {"type", "fullscreen"}, {"input", {"observed"}},
        {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
        {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}});
    config["rendering_passes"].push_back({{"name", "main_render"}, {"passes", passes}});
    auto root = TestSupport::renderGraphRoot(config);
    TestSupport::retainObserved(root, "main_render", {"observed"});
    writeTextFile(temp_dir / "hybrid.json", root.dump(2));
    configureRayQueryTestRuntime(temp_dir);
    auto &renderer = GET_MODULE(Renderer);
    renderer.render();
    GET_MODULE(VulkanManageCore).waitIdle();
    const auto image = RendererTestAccess::readR8RenderTarget(renderer, "observed");
    REQUIRE_FALSE(image.pixels.empty());
    CHECK(std::all_of(image.pixels.begin(), image.pixels.end(), [](auto p) { return p == 64; }));
    const auto id = GET_MODULE(RenderTargetContainer).getRenderTargetIdByName("observed");
    const auto metadata = GET_MODULE(RenderTargetContainer).getMetadata(id);
    CHECK(metadata.storage_mode == RenderTargetStorageMode::materialized);
    CHECK(bool(metadata.usage & ImageUsageBit::transfer_src));
    CHECK(metadata.samples == 1);
    const auto program = GET_MODULE(FrameGraphRuntimeContainer).findProgram(
        GET_MODULE(RenderingPassContainer).getRenderingPassIdByName("main_render"));
    REQUIRE(program != nullptr);
    const auto writer = std::find_if(program->rendering_pass.passes.begin(), program->rendering_pass.passes.end(),
        [](const auto &pass) { return pass.definition.name == "write_observed"; });
    REQUIRE(writer != program->rendering_pass.passes.end());
    CHECK(writer->definition.colorAttachmentOperations(0).store_op == AttachmentStoreOp::store);

    const auto refused = [&](std::string_view name) {
        CHECK_THROWS_WITH(RendererTestAccess::readRenderTarget(renderer, name, {}),
            Catch::Matchers::ContainsSubstring("render_target.readback_not_retained@1"));
        CHECK_THROWS_WITH(RendererTestAccess::readR8RenderTarget(renderer, name),
            Catch::Matchers::ContainsSubstring("render_target.readback_not_retained@1"));
    };
    refused("neighbor");
    REQUIRE_NOTHROW(RendererTestAccess::readRenderTarget(renderer, "observed", {}));
    REQUIRE_FALSE(GET_MODULE(RenderTarget).readbackLastFrameRGBA8().empty());
    const auto before = GET_MODULE(FrameGraphRuntimeContainer).snapshot();
    root["rendering"].erase("target_planning");
    writeTextFile(temp_dir / "hybrid.json", root.dump(2));
    REQUIRE(GET_MODULE(watch::ReloadService).applyRequestForTesting({.key = watch::makeAssetKey("hybrid.json")}));
    renderer.render();
    GET_MODULE(VulkanManageCore).waitIdle();
    CHECK(GET_MODULE(FrameGraphRuntimeContainer).snapshot() != before);
    refused("observed");
    std::filesystem::remove_all(temp_dir);
#else
    SKIP("runtime shader compiler unavailable");
#endif
}
