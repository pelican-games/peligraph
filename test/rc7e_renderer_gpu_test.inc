// rc7e_test_support.hpp and the renderingpass headers are included at the
// top of headless_render_test.cpp (this file is included inside its namespace).
TEST_CASE("RC-7e E2 registration allocates no target no graph uses", "[rc7e][rc7e-e2][gpu][headless]") {
#if PELICAN_RUNTIME_SHADER_COMPILER && PELICAN_WITH_OPENXR
    using Json = nlohmann::json;
    setupLogger(); const auto dir = makeTempProjectDir();
    FastModuleContainer modules; RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
    writeTextFile(dir/"scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
    writeTextFile(dir/"assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[]})");
    writeTextFile(dir/"shaders/rc7e_fullscreen.vert", gpuArenaFullscreenVertexShader());
    writeTextFile(dir/"shaders/rc7e_red.frag", aliasProducerAFragmentShader());
    writeTextFile(dir/"shaders/rc7e_green.frag", aliasProducerBFragmentShader());
    writeTextFile(dir/"shaders/rc7e_copy.frag", gpuArenaCopyFragmentShader());
    const TestSupport::Rc7eShaders shaders{"shaders/rc7e_fullscreen", "shaders/rc7e_red", "shaders/rc7e_green", "shaders/rc7e_copy"};
    const auto document = [&](const TestSupport::Rc7eOptions &options) {
        return TestSupport::renderGraphRoot(TestSupport::rc7eRenderingSection(dir, shaders, options)).dump(2);
    };
    writeTextFile(dir/"hybrid.json", document({})); configureRayQueryTestRuntime(dir);
    auto &launch = GET_MODULE(EngineLaunchConfig); launch.vulkan_validation = VulkanValidationMode::enabled; launch.gpu_labels = true;
    auto &core = GET_MODULE(VulkanManageCore); Rc7bValidationCapture validation{core};
    // Startup precompiles the XR variant next to the flat one.
    launch.xr_active = true;
    auto &renderer = GET_MODULE(Renderer); auto &targets = GET_MODULE(RenderTargetContainer); auto &output = GET_MODULE(RenderTarget);
    auto &runtime = GET_MODULE(FrameGraphRuntimeContainer); auto &passes = GET_MODULE(RenderingPassContainer);
    // The flat frame target's readbacks refuse while OpenXR is active: they
    // run with the flag lowered for the call only.
    const auto flatReadback = [&](auto read) {
        launch.xr_active = false;
        try { auto value = read(); launch.xr_active = true; return value; }
        catch (...) { launch.xr_active = true; throw; }
    };
    // The flat and XR variants in one transaction, replacing the startup scope.
    const auto variants = [&](RenderPipelineGpuRegistrationFaultPoint fault = RenderPipelineGpuRegistrationFaultPoint::none) {
        std::vector<RenderingPassConfigRegistrationDependencies> result;
        for (const auto variant : {RenderPipelineGraphVariant::flat, RenderPipelineGraphVariant::xr}) {
            RenderingPassConfigRegistrationDependencies::Options options;
            options.graph_variant = variant;
            options.gpu_owner_scope = renderPipelineVariantGpuOwnerScope(true, variant);
            options.publish_enabled_features = variant == RenderPipelineGraphVariant::flat;
            options.fault_point = fault;
            if (variant == RenderPipelineGraphVariant::xr)
                options.prepare_additional_gpu_resources = [] { Test::registerSyntheticXrMirrorTarget({32, 32}); };
            result.push_back(gpuArenaRegistrationDependencies(std::move(options)));
        }
        return result;
    };
    const auto select = [&](const char *graph) {
        RendererTestAccess::selectProgram(renderer, passes.getRenderingPassIdByName(graph));
    };
    const auto requireColor = [](const std::vector<std::uint8_t> &pixels, std::size_t expected_bytes, int channel) {
        REQUIRE(pixels.size() == expected_bytes);
        std::size_t wrong = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            for (int c = 0; c < 3; ++c) wrong += pixels[i + c] != (c == channel ? 255 : 0);
        CHECK(wrong == 0);
    };
    const auto renderFlat = [&](const char *graph, int channel, vk::Extent2D extent) {
        select(graph); GET_MODULE(EngineTime).advance(); renderer.render(); core.waitIdle();
        requireColor(flatReadback([&] { return output.readbackLastFrameRGBA8(); }), std::size_t{extent.width} * extent.height * 4, channel);
    };
    const auto memory = [&] { return renderer.currentFramePlanJson().at("memory_summary"); };
    const auto unallocatedNames = [&] {
        std::vector<std::string> names;
        const auto summary = memory();
        for (const auto &t : summary.at("unallocated_resources")) names.push_back(t.at("name").get<std::string>());
        return names;
    };

    // The reference: the same generation without the declarations no graph
    // uses. Registration allocated R0 (one image) and H (two surfaces) on
    // their own before RC-7e.
    registerRenderingPassConfigVariantsFromJsonData(document({.declare_unused = false}), {32, 32}, variants());
    select("main_render");
    const auto reference = memory();
    CHECK(reference.at("unallocated_resources") == Json::array());

    // A declaration error only registration checks is refused for a target
    // it does not register (a history image without SAMPLED).
    const auto before_refusal = runtime.snapshot();
    REQUIRE_THROWS_WITH(registerRenderingPassConfigVariantsFromJsonData(
        document({.history_usage = Json::array({"COLOR_ATTACHMENT"})}), {32, 32}, variants()),
        "History render target requires SAMPLED usage: H");
    REQUIRE(runtime.snapshot() == before_refusal);
    // So is a format the device cannot give the declared usage (the format
    // feature check before an image is made): this GPU's R8G8B8A8_SRGB has
    // no STORAGE_IMAGE, and S no graph uses.
    REQUIRE_FALSE(core.getPhysDevice().getFormatProperties(vk::Format::eR8G8B8A8Srgb).optimalTilingFeatures &
                  vk::FormatFeatureFlagBits::eStorageImage);
    REQUIRE_THROWS_WITH(registerRenderingPassConfigVariantsFromJsonData(
        document({.declare_srgb_storage = true}), {32, 32}, variants()),
        "Render target format lacks required color capability: S");
    REQUIRE(runtime.snapshot() == before_refusal);

    registerRenderingPassConfigVariantsFromJsonData(document({}), {32, 32}, variants());
    select("main_render");
    const auto summary = memory();
    CHECK(unallocatedNames() == std::vector<std::string>{"H", "R0"});
    for (const auto &t : summary.at("unallocated_resources")) {
        CHECK(t.at("reason") == "unused_in_generation");
        CHECK(t.at("payload_bytes") == (t.at("name") == "H" ? 2u : 1u) * 32u * 32u * 4u);
    }
    CHECK(summary.at("actual_allocation_count") == reference.at("actual_allocation_count"));
    CHECK(summary.at("actual_allocation_bytes") == reference.at("actual_allocation_bytes"));
    CHECK(summary.at("total_bytes") == reference.at("total_bytes"));
    for (const auto *name : {"R0", "H"}) CHECK_FALSE(isConcreteRenderTarget(targets.getRenderTargetIdByName(name)));
    for (const auto *name : {"A", "R1", "display"}) CHECK(isConcreteRenderTarget(targets.getRenderTargetIdByName(name)));
    const auto program = runtime.findProgram(passes.getRenderingPassIdByName("main_render"));
    REQUIRE(program);
    CHECK(program->frame_graph.unallocated_render_targets == std::set<std::string>{"H", "R0"});

    // A name lookup of a declared target the generation did not register:
    // the pass definition of a graph that uses R0.
    {
        const GraphTransformRegistrySnapshot transforms; const RenderStrategyRegistrySnapshot strategies;
        const SubgraphReplacementRegistrySnapshot replacements;
        const VulkanRenderCompilerBackendContext backend{vk::Format::eB8G8R8A8Srgb, {32, 32}, {}, {}, {},
            VulkanRenderCompilerDevicePlanningMode::compiler_only};
        const std::vector<RenderCompilerProgramVariantRequest> requests{{.graph_variant = RenderPipelineGraphVariant::flat}};
        const auto uses_r0 = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), {
            .rendering_config = Json::parse(document({.g1_uses_r0 = true})), .path_resolver = GET_MODULE(PathResolver),
            .runtime_shader_compiler_enabled = true, .graph_transforms = transforms, .render_strategies = strategies,
            .subgraph_replacements = replacements, .backend_context = backend, .variants = requests});
        const RenderTargetNameResolver names{targets, program->frame_graph.unallocated_render_targets};
        CHECK_THROWS_WITH(renderingPassDefinitionsOf(*uses_r0.variants.front().render_graph, uses_r0.generation_resources, names),
                          "render_target.not_allocated@1: R0");
        CHECK_THROWS_WITH(renderingPassDefinitionsOf(*uses_r0.variants.front().render_graph, uses_r0.generation_resources,
                                                     RenderTargetNameResolver{targets}),
                          Catch::Matchers::ContainsSubstring("unresolved_image"));
    }
    // The named readback keeps RC-7a's order.
    CHECK_THROWS_WITH(flatReadback([&] { return RendererTestAccess::readRenderTarget(renderer, "R0", {}); }),
                      Catch::Matchers::ContainsSubstring("render_target.readback_not_retained@1"));

    // Pixels: the flat and XR variants, descriptor rebinds through both
    // resize paths.
    renderFlat("main_render", 0, {32, 32});
    renderer.selectGraphVariant(RenderGraphVariant::xr);
    {
        Test::VulkanSyntheticStereoTarget stereo{{32, 32}, output.getSwapchainFormat()};
        GET_MODULE(EngineTime).advance(); renderer.renderLogicalFrame(stereo, rc3d2StereoViews()); core.waitIdle();
        requireColor(stereo.readback(0), 32u * 32u * 4u, 0);
    }
    renderer.selectGraphVariant(RenderGraphVariant::flat);
    renderFlat("main_render", 0, {32, 32});
    {
        auto old_output = test_support::RenderTargetTestAccess::resizeOffscreen(output, {48, 32}, false);
        renderFlat("main_render", 0, {48, 32});
    }
    CHECK(unallocatedNames() == std::vector<std::string>{"H", "R0"});
    // payload_bytes stays the estimate at the registration's extent (32x32),
    // not the container's current one.
    const auto resized = memory();
    for (const auto &t : resized.at("unallocated_resources"))
        CHECK(t.at("payload_bytes") == (t.at("name") == "H" ? 2u : 1u) * 32u * 32u * 4u);
    CHECK_FALSE(isConcreteRenderTarget(targets.getRenderTargetIdByName("R0")));
    {
        auto old_output = test_support::RenderTargetTestAccess::resizeOffscreen(output, {64, 48}, true);
        renderFlat("main_render", 0, {64, 48});
    }
    CHECK(unallocatedNames() == std::vector<std::string>{"H", "R0"});
    CHECK_FALSE(isConcreteRenderTarget(targets.getRenderTargetIdByName("R0")));

    // A failed replacement leaves the published list; a replacement whose g1
    // uses R0 makes R0 (and leaves A unregistered).
    const auto stable = runtime.snapshot();
    REQUIRE_THROWS_WITH(registerRenderingPassConfigVariantsFromJsonData(document({.g1_uses_r0 = true}), {64, 48},
        variants(RenderPipelineGpuRegistrationFaultPoint::after_runtime_prepare)),
        "Injected render pipeline GPU registration failure");
    REQUIRE(runtime.snapshot() == stable);
    CHECK(unallocatedNames() == std::vector<std::string>{"H", "R0"});
    CHECK_FALSE(isConcreteRenderTarget(targets.getRenderTargetIdByName("R0")));
    registerRenderingPassConfigVariantsFromJsonData(document({.g1_uses_r0 = true}), {64, 48}, variants());
    CHECK(isConcreteRenderTarget(targets.getRenderTargetIdByName("R0")));
    CHECK_FALSE(isConcreteRenderTarget(targets.getRenderTargetIdByName("A")));
    renderFlat("main_render", 0, {64, 48});
    CHECK(unallocatedNames() == std::vector<std::string>{"A", "H"});

    for (const auto &[phase, message] : validation.messages) { INFO(message); CHECK(message.empty()); }
    std::ofstream{"<作業ディレクトリ>/rc7e_e2.json"} << Json{{"reference", reference}, {"rc7e", summary}}.dump(2);
    std::filesystem::remove_all(dir);
#else
    SKIP("runtime shader compiler or OpenXR unavailable");
#endif
}
