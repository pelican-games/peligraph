namespace {
Rc6Json rc7aBloomFeature() {
    auto feature = Rc6Json::parse(PathResolver{}.loadText("engine://features/bloom.json"));
    for (auto &target : feature["render_targets"])
        if (target.at("name") == "Bloom_Threshold_RT") target["usage"].push_back("TRANSFER_SRC");
    feature["target_planning"] = rc6Policy("resources", "Bloom_Threshold_RT", "retain", true);
    return feature;
}
}
TEST_CASE("RC-7a F3 retain follows root recipe and part precedence and round trips", "[rendercompiler][rc7a][rc7-f3]") {
    auto root = rc5Document(true, true, false);
    root["rendering"]["features"].push_back("project://features/local.json");
    auto feature = rc7aBloomFeature();
    const auto retained = rc5Compile(root, false, &feature);
    const auto &plan = *requireVulkanRenderCompilerPhysicalPackage(*retained.variants[0].physical_package).target_plans.at("main_render");
    REQUIRE_NOTHROW(validateTargetReadbackRetention(&plan, "Bloom_Threshold_RT", "main_render"));
    CHECK(rc6Row(rc6Physical(retained), "Bloom_Threshold_RT", "part").at("status") == "adopted");
    const auto serialized = serializeCompiledRenderPipelineMetadata(*retained.variants[0].compiled_pipeline);
    const auto parsed = parseTargetPlanningPolicy(serialized);
    REQUIRE_FALSE(parsed.graphs.empty());
    const auto &rows = parsed.graphs[0].declarations;
    CHECK(std::any_of(rows.begin(), rows.end(), [](const auto &e) { return e.key == "retain" && std::get<bool>(e.value); }));
    root["rendering"]["target_planning"] = rc6Policy("resources", "Bloom_Threshold_RT", "retain", false);
    const auto canceled = rc5Compile(root, false, &feature);
    const auto &canceled_plan = *requireVulkanRenderCompilerPhysicalPackage(*canceled.variants[0].physical_package).target_plans.at("main_render");
    CHECK(rc6Row(rc6Physical(canceled), "Bloom_Threshold_RT", "part").at("status") == "overridden");
    CHECK(rc6Row(rc6Physical(canceled), "Bloom_Threshold_RT", "root").at("value") == false);
    CHECK(rc6Refusal([&] { validateTargetReadbackRetention(&canceled_plan, "Bloom_Threshold_RT", "main_render"); }) == "render_target.readback_not_retained@1");
    root["rendering"].erase("target_planning");
    root["rendering"]["pipeline"]["preset"] = "project://recipe.json";
    auto recipe = Rc6Json::parse(PathResolver{}.loadText("engine://render_pipelines/experimental/hybrid_recipe.json"));
    auto fragment = Rc6Json::parse(PathResolver{}.loadText("engine://render_fragments/deferred_lighting.json"));
    fragment["target_planning"] = rc6Policy("resources", "lit_color", "retain", true);
    for (auto &base : recipe["config"]["base"]) for (auto &ref : base["fragments"])
        if (ref == "engine://render_fragments/deferred_lighting.json") ref = "project://lighting_part.json";
    recipe["config"]["target_planning"] = rc6Policy("resources", "lit_color", "retain", false);
    const auto recipe_result = rc6Physical(rc5Compile(root, false, &feature,
        {{"recipe.json", recipe}, {"lighting_part.json", fragment}}));
    CHECK(rc6Row(recipe_result, "lit_color", "recipe").at("value") == false);
    CHECK(rc6Row(recipe_result, "lit_color", "part").at("status") == "overridden");

}
TEST_CASE("RC-7a F3 retain ownership absence and complete exclusion", "[rendercompiler][rc7a][rc7-f3]") {
    auto root = rc5Document(true, true, false);
    root["rendering"]["features"].push_back("project://features/local.json");
    auto feature = rc7aBloomFeature();
    feature["target_planning"] = rc6Policy("resources", "lit_color", "retain", true);
    CHECK(rc6Refusal([&] { rc5Compile(root, false, &feature); }) == "target_planning.policy_foreign_name@1");
    feature.erase("target_planning");
    root["rendering"]["target_planning"] = rc6Policy("resources", "gone", "retain", true);
    const auto absent = rc6Physical(rc5Compile(root, false, &feature));
    CHECK(rc6Row(absent, "gone", "root").at("status") == "absent");
    root["rendering"].erase("target_planning");
    const auto automatic = rc5Compile(root, false, &feature);
    const auto &automatic_plan = *requireVulkanRenderCompilerPhysicalPackage(*automatic.variants[0].physical_package).target_plans.at("main_render");
    root["rendering"]["target_planning"] = rc6Policy("resources", "lit_color", "retain", true);
    const auto compiled = rc5Compile(root, false, &feature);
    auto package = requireVulkanRenderCompilerPhysicalPackage(*compiled.variants[0].physical_package);
    CHECK(rc6Refusal([&] { installVerifiedVulkanCompletePhysicalPlanPackage(package, "main_render",
        ejectVulkanCompletePhysicalPlanPackage(automatic_plan)); }) == "target_planning.policy_with_legacy_package@1");
}
TEST_CASE("RC-7a F2 variant merged usage reaches retain planning", "[rendercompiler][rc7a][rc7-f2]") {
#if PELICAN_WITH_OPENXR
    ProgramInputFixture fixture;
    fixture.config = rc5Document(false, false, false);
    fixture.config["nodes"] = Rc6Json::array({fg1Image("retained_image"), fg1Fill("fill_retained", "retained_image")});
    fixture.config["rendering"]["target_planning"] = rc6Policy("resources", "retained_image", "retain", true);
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64,64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat},
        {.graph_variant = RenderPipelineGraphVariant::xr, .compose_runtime_config = [](nlohmann::json &config) {
            for (auto &target : config["render_targets"])
                if (target.at("name") == "retained_image") target["usage"].push_back("TRANSFER_SRC");
        }}};
    const auto result = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), fixture.input());
    REQUIRE(result.variants.size() == 2);
    for (const auto &variant : result.variants) {
        const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package);
        REQUIRE(physical.target_plans.size() == 1);
        const auto &plan = *physical.target_plans.begin()->second;
        CHECK(bool(plan.effective_image_usage.at("retained_image") & ImageUsageBit::transfer_src));
        CHECK_NOTHROW(validateTargetReadbackRetention(&plan, "retained_image", plan.graph));
    }
#else
    SKIP("XR variant compiler unavailable");
#endif
}
TEST_CASE("RC-7a F5 observation helper merges only declared leaves and checks exact sets", "[rendercompiler][rc7a][rc7-f5]") {
    auto root = TestSupport::renderGraphRoot(Rc6Json::object());
    root["rendering"]["target_planning"]["graphs"]["main"]["backend"] = "materialized";
    root["rendering"]["target_planning"]["graphs"]["main"]["resources"]["observed"]["no_alias"] = true;
    TestSupport::retainObserved(root, "main", {"observed"});
    const auto &declared = root["rendering"]["target_planning"]["graphs"]["main"];
    CHECK(declared.at("backend") == "materialized");
    CHECK(declared.at("resources").size() == 1);
    CHECK(declared.at("resources").at("observed") == Rc6Json{{"no_alias", true}, {"retain", true}});
    CHECK_THROWS_AS(TestSupport::retainObserved(root, "main", {"observed", "observed"}), std::logic_error);
    root["rendering"]["target_planning"]["graphs"]["main"]["resources"]["observed"]["retain"] = false;
    CHECK_THROWS_AS(TestSupport::retainObserved(root, "main", {"observed"}), std::logic_error);
    root["rendering"]["target_planning"]["graphs"]["main"]["resources"]["observed"]["retain"] = true;
    root["rendering"]["target_planning"]["graphs"]["main"]["resources"]["unobserved"]["retain"] = true;
    CHECK_THROWS_AS(TestSupport::retainObserved(root, "main", {"observed"}), std::logic_error);
}
TEST_CASE("RC-7a F3 replacing a same-name owner does not carry its retain", "[rendercompiler][rc7a][rc7-f3]") {
    auto fragment = Rc6Json::parse(PathResolver{}.loadText("engine://render_fragments/deferred_lighting.json"));
    fragment["target_planning"] = rc6Policy("resources", "lit_color", "retain", true);
    auto replacement = fragment;
    replacement.erase("target_planning");
    auto recipe = Rc6Json::parse(PathResolver{}.loadText("engine://render_pipelines/experimental/hybrid_recipe.json"));
    for (auto &base : recipe["config"]["base"]) for (auto &ref : base["fragments"])
        if (ref == "engine://render_fragments/deferred_lighting.json") ref = "project://owner.json";
    auto root = rc5Document(false, false, false);
    root["rendering"]["pipeline"]["preset"] = "project://recipe.json";
    const auto compile = [&] { return rc5Compile(root, false, nullptr,
        {{"recipe.json", recipe}, {"owner.json", fragment}, {"replacement.json", replacement}}); };
    const auto before = compile();
    const auto &first = *requireVulkanRenderCompilerPhysicalPackage(*before.variants[0].physical_package).target_plans.at("main_render");
    REQUIRE_NOTHROW(validateTargetReadbackRetention(&first, "lit_color", "main_render"));
    root["rendering"]["fragment_overrides"] = {{"deferred_lighting", "project://replacement.json"}};
    const auto after = compile();
    const auto &second = *requireVulkanRenderCompilerPhysicalPackage(*after.variants[0].physical_package).target_plans.at("main_render");
    CHECK(second.policy.empty());
    CHECK(rc6Refusal([&] { validateTargetReadbackRetention(&second, "lit_color", "main_render"); }) ==
        "render_target.readback_not_retained@1");
}

TEST_CASE("RC-7a fix generation usage includes XR depth export", "[rendercompiler][rc7a][rc7a-fix]") {
#if PELICAN_WITH_OPENXR
    ProgramInputFixture fixture;
    fixture.config = rc5Document(false, false, false);
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64,64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat},
        {.graph_variant = RenderPipelineGraphVariant::xr, .enable_external_depth_export = true}};
    for (auto &request : fixture.variants) request.compose_runtime_config = [](nlohmann::json &config) {
        for (auto &target : config["render_targets"]) {
            auto &usage = target["usage"];
            if (std::find(usage.begin(), usage.end(), "DEPTH_STENCIL_ATTACHMENT") != usage.end()) {
                usage.erase(std::remove(usage.begin(), usage.end(), "TRANSFER_SRC"), usage.end());
            }
        }
    };
    auto input = fixture.input();
    input.runtime_shader_compiler_enabled = true;
    const auto automatic = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
    const auto &xr = requireVulkanRenderCompilerPhysicalPackage(*automatic.variants[1].physical_package);
    REQUIRE_FALSE(xr.target_plans.empty());
    const auto &xr_plan = *xr.target_plans.begin()->second;
    REQUIRE(xr_plan.external_depth_export);
    const auto name = xr_plan.external_depth_export->source_resource;
    REQUIRE_FALSE(bool(findConfigImage(*automatic.variants[0].render_graph, name)->usage & ImageUsageBit::transfer_src));
    fixture.config["rendering"]["target_planning"] = rc6Policy("resources", name.c_str(), "retain", true);
    const auto retained = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
    const auto *registered = retained.generation_resources.findImage(name);
    REQUIRE(registered);
    CHECK(bool(registered->usage & ImageUsageBit::transfer_src));
    for (const auto &variant : retained.variants) {
        const auto &plan = *requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package).target_plans.begin()->second;
        CHECK(plan.effective_image_usage.at(name) == registered->usage);
        CHECK_NOTHROW(validateTargetReadbackRetention(&plan, name, plan.graph));
    }
    // XR intentionally rejects history. Verify that engine addition through
    // a legal flat history fixture, using its original declared transfer source.
    fixture.variants.resize(1);
    fixture.variants[0].compose_runtime_config = [name](nlohmann::json &config) {
        for (auto &target : config["render_targets"])
            if (target.at("name") == name) target["history"] = true;
    };
    auto history_input = fixture.input();
    history_input.runtime_shader_compiler_enabled = true;
    const auto history = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), history_input);
    const auto *history_image = history.generation_resources.findImage(name);
    REQUIRE(history_image);
    CHECK(history_image->declaration.history);
    CHECK(bool(history_image->usage & ImageUsageBit::transfer_dst));
    const auto &history_plan = *requireVulkanRenderCompilerPhysicalPackage(*history.variants[0].physical_package).target_plans.begin()->second;
    CHECK(history_plan.effective_image_usage.at(name) == history_image->usage);
#else
    SKIP("XR variant compiler unavailable");
#endif
}

TEST_CASE("RC-7a fix observations reject an extra declared retain", "[rendercompiler][rc7a][rc7a-fix]") {
    auto root = TestSupport::renderGraphRoot(Rc6Json::object());
    // Two declarations, but only one successful readback in this fixture.
    const std::vector<std::string> declared{"observed", "extra"};
    const std::vector<std::string> successful{"observed"};
    TestSupport::retainObserved(root, "main", declared);
    CHECK_THROWS_AS(TestSupport::validateRetainObservations(root, "main", successful), std::logic_error);
}

TEST_CASE("RC-7a fix observation audit separates fixtures graphs and variants and detects zero reads", "[rendercompiler][rc7a][rc7a-fix]") {
    TestSupport::RetainObservationAudit audit;
    const TestSupport::RetainObservationAudit::Key first{"fixture1", "main", 0};
    audit.declare(first, {"a", "b"});
    CHECK(audit.mismatches().size() == 1);
    audit.record(first, {"a", "b"}, "a");
    audit.record(first, {"a", "b"}, "a");
    CHECK(audit.mismatches().size() == 1);
    audit.record(first, {"a", "b"}, "b");
    CHECK(audit.mismatches().empty());
    for (const auto &key : {TestSupport::RetainObservationAudit::Key{"fixture2", "main", 0},
                           TestSupport::RetainObservationAudit::Key{"fixture1", "other", 0},
                           TestSupport::RetainObservationAudit::Key{"fixture1", "main", 1}})
        audit.record(key, {"a", "b"}, "b");
    CHECK(audit.mismatches().size() == 3);
}
