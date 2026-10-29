namespace {
Rc6Json rc6b1CompilerShape(const Rc6Json &plan) {
    Rc6Json result{{"backend", plan.at("backend_selection").at("selected_candidate")}, {"scopes", Rc6Json::array()}, {"aliases", Rc6Json::array()}, {"representations", Rc6Json::object()}};
    for (const auto &scope : plan.at("scopes")) result["scopes"].push_back(scope.at("nodes"));
    for (const auto &alias : plan.at("alias_groups")) result["aliases"].push_back(alias.at("resources"));
    for (const auto &resource : plan.at("resources")) result["representations"][resource.at("logical_resource").get<std::string>()] = resource.at("representation");
    return result;
}
Rc6Json rc6b1CompilerFreeze(const RenderCompilerProgramResult &compiled, const RenderCompilerProgramResult &editable, const char *graph) {
    std::set<std::string> names;
    std::map<std::string, std::string> authored;
    for (const auto &node : editable.variants.at(0).compiled_pipeline->pass_provenance) {
        authored[node.name] = node.authored_name.empty() ? node.name : node.authored_name;
        names.insert(authored.at(node.name));
    }
    return PelicanStudio::freezeFramePlanPolicy(rc6Physical(compiled, graph),
        framePlanOrder(compiled.variants.at(0).frame_plans.at(graph)), names, {}, Rc6Json::object(), authored);
}
}
TEST_CASE("RC-6b1 D2 Freeze XR compute author names bind both flat and XR", "[rendercompiler][rc6b1][rc6-d2-xr]") {
#if PELICAN_WITH_OPENXR
    auto root = rc5Document(false, false, false);
    root["rendering"]["features"] = {"project://features/local.json"};
    auto feature = rc6TaskOwner();
    const auto xr = rc5Compile(root, true, &feature);
    const auto flat = rc5Compile(root, false, &feature);
    const auto frozen = rc6b1CompilerFreeze(xr, xr, "main_render#xr");
    REQUIRE(frozen.at("nodes").contains("probe_fill"));
    CHECK_FALSE(frozen.at("nodes").contains("probe_fill#xr"));
    root["rendering"]["target_planning"]["graphs"]["main_render"] = frozen;
    const auto xr_after = rc6Physical(rc5Compile(root, true, &feature), "main_render#xr");
    const auto flat_after = rc6Physical(rc5Compile(root, false, &feature));
    CHECK(rc6b1CompilerShape(xr_after) == rc6b1CompilerShape(rc6Physical(xr, "main_render#xr")));
    CHECK(rc6b1CompilerShape(flat_after) == rc6b1CompilerShape(rc6Physical(flat)));
    CHECK(rc6Row(xr_after, "probe_fill#xr", "root").at("satisfied") == true);
    CHECK(rc6Row(flat_after, "probe_fill", "root").at("satisfied") == true);
#else
    SKIP("OpenXR disabled");
#endif
}
TEST_CASE("RC-6b1 D2 runtime ImGui Freeze skips its name and preserves the plan", "[rendercompiler][rc6b1][rc6-d2-imgui]") {
#if PELICAN_WITH_IMGUI
    auto root = rc5Document(false, false, false);
    const auto editable = rc5Compile(root, false);
    const auto compile_runtime = [&] {
        ProgramInputFixture fixture;
        fixture.config = root;
        fixture.backend_context = VulkanRenderCompilerBackendContext{
            vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {}, VulkanRenderCompilerDevicePlanningMode::compiler_only};
        fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat,
            .compose_runtime_config = [](nlohmann::json &config) { appendImGuiPassToCanonicalGraphs(config); }}};
        auto input = fixture.input();
        input.runtime_shader_compiler_enabled = true;
        return runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
    };
    const auto runtime = compile_runtime();
    const auto before = rc6Physical(runtime);
    const auto order = framePlanOrder(runtime.variants.at(0).frame_plans.at("main_render"));
    REQUIRE(std::find(order.begin(), order.end(), "imgui_pass") != order.end());
    const auto frozen = rc6b1CompilerFreeze(runtime, editable, "main_render");
    CHECK_FALSE(frozen.at("nodes").contains("imgui_pass"));
    root["rendering"]["target_planning"]["graphs"]["main_render"] = frozen;
    CHECK(rc6b1CompilerShape(rc6Physical(compile_runtime())) == rc6b1CompilerShape(before));
#else
    SKIP("ImGui disabled");
#endif
}
