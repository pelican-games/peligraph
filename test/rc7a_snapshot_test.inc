TEST_CASE("RC-7a F1 snapshot program and generation never inherit another readback permission", "[frame-plan][rc7a][rc7-f1]") {
    FastModuleContainer modules;
    auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
    const auto publish = [&](bool retain) {
        std::vector<RenderPipelineProgramPreparation> programs;
        for (int i = 0; i < 3; ++i) {
            const auto name = i == 0 ? "retained_graph" : i == 1 ? "plain_graph" : "plain_graph#xr";
            auto plan = std::make_shared<VulkanTargetPlan>();
            plan->graph = name;
            plan->resources.push_back({.logical_resource = "same_image", .format = ImageFormat::r8_unorm,
                .lifetime = {.used = true, .retain = i == 0 && retain ? std::vector<std::string>{"readback"} :
                    i == 1 ? std::vector<std::string>{"persistent", "picking"} : std::vector<std::string>{}}});
            CompiledRenderingPass pass;
            pass.name = name;
            FramePlan frame;
            frame.name = name;
            programs.push_back({.rendering_pass = pass, .frame_plan = frame,
                .render_pipeline = std::make_shared<CompiledRenderPipeline>(), .target_plan = plan,
                .render_target_bindings = {{"same_image", GlobalRenderTargetId{1}}}, .rendering_pass_id = RenderingPassId{71 + i}});
        }
        runtime.publishPreparedGeneration(runtime.prepareGeneration(std::move(programs)));
    };
    publish(true);
    const auto first = runtime.snapshot();
    Renderer renderer{RendererFramePlanCpuSeam{}, RenderingPassId{71}};
    const auto refused = [&] {
        CHECK_THROWS_WITH(test_support::RendererTestAccess::readRenderTarget(renderer, "same_image", {}),
            Catch::Matchers::ContainsSubstring("render_target.readback_not_retained@1"));
        CHECK_THROWS_WITH(test_support::RendererTestAccess::readR8RenderTarget(renderer, "same_image"),
            Catch::Matchers::ContainsSubstring("render_target.readback_not_retained@1"));
    };
    REQUIRE_NOTHROW(validateTargetReadbackRetention(first->find(RenderingPassId{71})->frame_graph.target_plan.get(), "same_image", "retained_graph"));
    test_support::RendererTestAccess::selectProgram(renderer, RenderingPassId{72});
    refused();
    test_support::RendererTestAccess::selectProgram(renderer, RenderingPassId{73});
    refused();
    test_support::RendererTestAccess::selectProgram(renderer, RenderingPassId{71});
    publish(false);
    CHECK(runtime.snapshot() != first);
    refused();
    // Holding the old snapshot cannot grant permission to the new program.
    REQUIRE_NOTHROW(validateTargetReadbackRetention(first->find(RenderingPassId{71})->frame_graph.target_plan.get(), "same_image", "retained_graph"));
}

TEST_CASE("RC-7a fix missing pipeline keeps the readback prerequisite diagnostic", "[frame-plan][rc7a][rc7a-fix]") {
    FastModuleContainer modules;
    CHECK(TestSupport::declareActiveRetainFixture != nullptr);
    Renderer renderer{RendererFramePlanCpuSeam{}, RenderingPassId{987654}};
    CHECK_THROWS_WITH(RendererTestAccess::readRenderTarget(renderer, "any_image", {}),
        "render-target readback requires a compiled render pipeline");
    CHECK_THROWS_WITH(RendererTestAccess::readR8RenderTarget(renderer, "any_image"),
        "render-target readback requires a compiled render pipeline");
}
