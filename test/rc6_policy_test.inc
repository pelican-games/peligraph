// Production compiler acceptance for RC-6a1 (no Vulkan device required).
namespace {
using Rc6Json = nlohmann::json;
Rc6Json rc6Policy(const char *kind, const char *target, const char *key, Rc6Json value,
                 const char *graph = "main_render") {
    return {{"graphs", {{graph, {{kind, {{target, {{key, std::move(value)}}}}}}}}}};
}
Rc6Json rc6Physical(const RenderCompilerProgramResult &result, const char *graph = "main_render") {
    return vulkanTargetPlanToJson(*requireVulkanRenderCompilerPhysicalPackage(
        *result.variants.at(0).physical_package).target_plans.at(graph));
}
bool rc6Aliased(const Rc6Json &plan, const char *a, const char *b) {
    for (const auto &group : plan.at("alias_groups")) {
        const auto &r = group.at("resources");
        if (std::find(r.begin(), r.end(), a) != r.end() && std::find(r.begin(), r.end(), b) != r.end()) return true;
    }
    return false;
}
bool rc6InRegion(const Rc6Json &plan,const char *name) {
    for(const auto &region:plan.at("memory_regions"))for(const auto &r:region.at("residents"))if(r.at("resource")==name)return true;
    return false;
}
std::string rc6Refusal(const std::function<void()> &operation) {
    try { operation(); } catch (const GraphRegistryError &e) { return e.code(); }
    return {};
}
Rc6Json rc6Row(const Rc6Json &physical, const char *target, const char *kind) {
    for (const auto &row : physical.at("policy"))
        if (row.at("target") == target && row.at("origin").at("kind") == kind) return row;
    FAIL("missing policy row");
    return {};
}
}

TEST_CASE("RC-6 B1 B2 feature no_alias changes region admission and root false restores it", "[rendercompiler][rc6a1][rc6-b1][rc6-b2]") {
    auto root = rc5Document(true, true, false);
    root["rendering"]["features"].push_back("project://features/local.json");
    auto feature = Rc6Json::parse(PathResolver{}.loadText("engine://features/bloom.json"));
    const auto automatic = rc6Physical(rc5CompileWithFacts(root, false, &feature));
    REQUIRE(rc6InRegion(automatic, "Bloom_Threshold_RT"));
    REQUIRE_FALSE(automatic.contains("policy"));
    feature["target_planning"] = rc6Policy("resources", "Bloom_Threshold_RT", "no_alias", true);
    const auto hinted = rc6Physical(rc5CompileWithFacts(root, false, &feature));
    REQUIRE_FALSE(rc6InRegion(hinted, "Bloom_Threshold_RT"));
    auto row = rc6Row(hinted, "Bloom_Threshold_RT", "part");
    CHECK(row.at("strength") == "prefer");
    CHECK(row.at("status") == "adopted");
    CHECK(row.at("origin").at("document") == "project://features/local.json");
    CHECK(row.at("origin").at("pointer") == "/target_planning/graphs/main_render/resources/Bloom_Threshold_RT/no_alias");
    CHECK_FALSE(row.contains("satisfied"));
    root["rendering"]["target_planning"] = rc6Policy("resources", "Bloom_Threshold_RT", "no_alias", false);
    const auto overridden = rc6Physical(rc5CompileWithFacts(root, false, &feature));
    REQUIRE(rc6InRegion(overridden, "Bloom_Threshold_RT"));
    CHECK(rc6Row(overridden, "Bloom_Threshold_RT", "part").at("status") == "overridden");
    CHECK(rc6Row(overridden, "Bloom_Threshold_RT", "root").at("value") == false);
    // Policy is observation, not identity, when its result is unchanged.
    CHECK(overridden.at("automatic_plan_fingerprint") == automatic.at("automatic_plan_fingerprint"));
    CHECK(overridden.at("logical_graph_fingerprint") == automatic.at("logical_graph_fingerprint"));
}

TEST_CASE("RC-6 B3 B10 recipe priority and selected fragment provenance", "[rendercompiler][rc6a1][rc6-b3][rc6-b10]") {
    auto fragment = Rc6Json::parse(PathResolver{}.loadText("engine://render_fragments/deferred_lighting.json"));
    fragment["target_planning"] = rc6Policy("nodes", "deferred_lighting", "serial", true);
    auto recipe = Rc6Json::parse(PathResolver{}.loadText("engine://render_pipelines/experimental/hybrid_recipe.json"));
    for (auto &base : recipe["config"]["base"]) for (auto &ref : base["fragments"])
        if (ref == "engine://render_fragments/deferred_lighting.json") ref = "project://original.json";
    recipe["config"]["target_planning"] = rc6Policy("nodes", "deferred_lighting", "serial", false);
    auto root = rc5Document(false, false, false);
    root["rendering"]["pipeline"]["preset"] = "project://recipe.json";
    const auto compile = [&](const Rc6Json &r, const Rc6Json &replacement) {
        return rc5CompileWithFacts(r, false, nullptr, {{"recipe.json", recipe}, {"original.json", fragment}, {"replacement.json", replacement}});
    };
    const auto compiled = compile(root, fragment);
    const auto &constraints = compiled.variants.at(0).compiled_pipeline->target_planning.graphs.at(0).nodes;
    const auto effective = std::find_if(constraints.begin(), constraints.end(), [](const auto &n) { return n.node == "deferred_lighting"; });
    REQUIRE(effective != constraints.end());
    REQUIRE(effective->serial.declaration.has_value());
    CHECK_FALSE(static_cast<bool>(effective->serial));
    CHECK(effective->serial.declaration->origin.kind == PlanningOriginKind::recipe);
    const auto plan = rc6Physical(compiled);
    CHECK(rc6Row(plan, "deferred_lighting", "recipe").at("value") == false);
    CHECK(rc6Row(plan, "deferred_lighting", "recipe").at("strength") == "prefer");
    CHECK(rc6Row(plan, "deferred_lighting", "part").at("status") == "overridden");
    auto replacement = fragment;
    replacement.erase("target_planning");
    root["rendering"]["fragment_overrides"] = {{"deferred_lighting", "project://replacement.json"}};
    const auto selected = rc6Physical(compile(root, replacement));
    REQUIRE(selected.at("policy").size() == 1);
    CHECK(selected.at("policy")[0].at("origin").at("kind") == "recipe");
    root["rendering"]["target_planning"] = rc6Policy("nodes", "deferred_lighting", "serial", true);
    const auto root_plan = rc6Physical(compile(root, replacement));
    CHECK(rc6Row(root_plan, "deferred_lighting", "recipe").at("status") == "overridden");
    CHECK(rc6Row(root_plan, "deferred_lighting", "root").at("value") == true);
}

TEST_CASE("RC-6 B4 feature cannot constrain a borrowed base resource", "[rendercompiler][rc6a1][rc6-b4]") {
    auto root = rc5Document(false, false, false);
    root["rendering"]["features"] = {"project://features/local.json"};
    auto feature = Rc6Json::parse(PathResolver{}.loadText("engine://features/bloom.json"));
    feature["target_planning"] = rc6Policy("resources", "lit_color", "no_alias", true);
    CHECK(rc6Refusal([&] { rc5CompileWithFacts(root, false, &feature); }) == "target_planning.policy_foreign_name@1");
}

TEST_CASE("RC-6 B5 engine isolation rejects require and explains discarded prefer", "[rendercompiler][rc6a1][rc6-b5]") {
    auto root = rc5Document(false, false, false);
    root["rendering"]["features"] = {"engine://features/sprite.json"};
    const auto automatic = rc6Physical(rc5CompileWithFacts(root, false));
    REQUIRE_FALSE(automatic.contains("policy"));
    root["rendering"]["target_planning"] = rc6Policy("nodes", "sprite_draw", "isolate", {{"value", false}, {"strength", "require"}});
    CHECK(rc6Refusal([&] { rc5CompileWithFacts(root, false); }) == "target_planning.policy_conflicts_engine@1");
    root["rendering"]["target_planning"]["graphs"]["main_render"]["nodes"]["sprite_draw"]["isolate"]["strength"] = "prefer";
    const auto preferred = rc6Physical(rc5CompileWithFacts(root, false));
    const auto row = rc6Row(preferred, "sprite_draw", "root");
    CHECK(row.at("status") == "engine_forced");
    CHECK_FALSE(row.contains("satisfied"));
    CHECK(preferred.at("automatic_plan_fingerprint") == automatic.at("automatic_plan_fingerprint"));
}

TEST_CASE("RC-6 B6 deleted root target warns and retains its absent declaration", "[rendercompiler][rc6a1][rc6-b6]") {
    auto root = rc5Document(false, false, true);
    root["rendering"]["target_planning"] = rc6Policy("nodes", "probe_fill", "serial", true);
    root["nodes"].erase(root["nodes"].begin() + 1);
    const auto compiled = rc5CompileWithFacts(root, false);
    const auto &pipeline = *compiled.variants.at(0).compiled_pipeline;
    CHECK(std::any_of(pipeline.diagnostics.begin(), pipeline.diagnostics.end(), [](const auto &d) {
        return d.kind == RenderPipelineDiagnosticKind::planning_name_absent && d.subject == "probe_fill";
    }));
    CHECK(rc6Row(rc6Physical(compiled), "probe_fill", "root").at("status") == "absent");
}

TEST_CASE("RC-6 B9 policy rejects complete installs", "[rendercompiler][rc6a1][rc6-b9]") {
    auto root = rc5Document(false, false, false);
    const auto automatic = rc5CompileWithFacts(root, false);
    const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*automatic.variants.at(0).physical_package);
    const auto &plan = *physical.target_plans.at("main_render");
    root["rendering"]["target_planning"] = rc6Policy("nodes", "forward_opaque", "serial", true);


    SECTION("complete") {
        auto compiled = rc5CompileWithFacts(root, false);
        auto package = requireVulkanRenderCompilerPhysicalPackage(*compiled.variants.at(0).physical_package);
        CHECK(rc6Refusal([&] { installVerifiedVulkanCompletePhysicalPlanPackage(package, "main_render", ejectVulkanCompletePhysicalPlanPackage(plan)); }) ==
            "target_planning.policy_with_legacy_package@1");
    }
}


namespace {
bool rc6ParallelWith(const Rc6Json &plan, std::string_view node) {
    const auto &pairs = plan.at("planning_opportunities").at("parallel_candidates");
    return std::any_of(pairs.begin(), pairs.end(), [&](const auto &p) {
        return p.at("first").template get<std::string>() == node || p.at("second").template get<std::string>() == node;
    });
}
}

TEST_CASE("RC-6 B3 serial changes physical parallel opportunities and recipe false restores them", "[rendercompiler][rc6a1][rc6-fix-b3]") {
    auto root = rc5sIndependentDocument();
    auto config = root.at("rendering");
    Rc6Json fragment{{"schema", "pelican.render_fragment"}, {"version", 1}, {"name", "independent"},
        {"render_targets", config.at("render_targets")}, {"passes", config["rendering_passes"][0]["passes"]}};
    Rc6Json recipe{{"schema", "pelican.render_pipeline"}, {"version", 2}, {"name", "parallel"},
        {"config", {{"base", Rc6Json::array({{{"family", "main"}, {"fragments", {"project://part.json"}}}})}}}};
    root["rendering"] = {{"pipeline", {{"preset", "project://recipe.json"}}}};
    const auto compile = [&] { return rc6Physical(rc5CompileWithFacts(root, false, nullptr,
        {{"recipe.json", recipe}, {"part.json", fragment}}), "main"); };
    const auto automatic = compile();
    REQUIRE(rc6ParallelWith(automatic, "scene"));
    fragment["target_planning"] = rc6Policy("nodes", "scene", "serial", true, "main");
    const auto restricted = compile();
    REQUIRE_FALSE(rc6ParallelWith(restricted, "scene"));
    recipe["config"]["target_planning"] = rc6Policy("nodes", "scene", "serial", false, "main");
    const auto restored = compile();
    REQUIRE(rc6ParallelWith(restored, "scene"));
    CHECK(restored.at("planning_opportunities").at("parallel_candidates") == automatic.at("planning_opportunities").at("parallel_candidates"));
    CHECK(rc6Row(restored, "scene", "part").at("status") == "overridden");
}

TEST_CASE("RC-6 final XR task names bind root and part policy to the physical plan", "[rendercompiler][rc6a1][rc6-fix-xr]") {
#if PELICAN_WITH_OPENXR
    auto root = rc5Document(false, false, false);
    root["rendering"]["features"] = {"project://features/local.json"};
    auto feature = Rc6Json::parse(PathResolver{}.loadText("engine://features/ui.json"));
    feature["name"] = "task_owner";
    feature.erase("passes");
    // A second, independent per-frame task is owned by this feature.
    feature["compute_tasks"] = Rc6Json::array({{{"name", "owned_task"}, {"shader", "engine://shaders/compute/clustered_light_select"},
        {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}}});
    auto probe = feature["compute_tasks"][0]; probe["name"] = "probe_fill";
    feature["compute_tasks"].push_back(probe);
    const auto compile = [&] { return rc5CompileWithFacts(root, true, &feature); };
    const auto automatic = rc6Physical(compile(), "main_render#xr");
    REQUIRE(rc6ParallelWith(automatic, "probe_fill#xr"));
    REQUIRE(rc6ParallelWith(automatic, "owned_task#xr"));
    SECTION("root final name") {
        root["rendering"]["target_planning"] = rc6Policy("nodes", "probe_fill#xr", "serial", true);
        const auto result = compile();
        const auto plan = rc6Physical(result, "main_render#xr");
        REQUIRE_FALSE(rc6ParallelWith(plan, "probe_fill#xr"));
        CHECK(rc6Row(plan, "probe_fill#xr", "root").at("status") == "adopted");
        CHECK(std::none_of(result.variants[0].compiled_pipeline->diagnostics.begin(), result.variants[0].compiled_pipeline->diagnostics.end(), [](const auto &d) {
            return d.kind == RenderPipelineDiagnosticKind::planning_name_absent;
        }));
    }
    SECTION("part authored name and root final name compete on one binding") {
        feature["target_planning"] = rc6Policy("nodes", "owned_task", "serial", true);
        const auto restricted = rc6Physical(compile(), "main_render#xr");
        REQUIRE_FALSE(rc6ParallelWith(restricted, "owned_task#xr"));
        CHECK(rc6Row(restricted, "owned_task#xr", "part").at("status") == "adopted");
        root["rendering"]["target_planning"] = rc6Policy("nodes", "owned_task#xr", "serial", false);
        const auto restored = rc6Physical(compile(), "main_render#xr");
        REQUIRE(rc6ParallelWith(restored, "owned_task#xr"));
        CHECK(rc6Row(restored, "owned_task#xr", "part").at("status") == "overridden");
    }
#else
    FAIL("XR build required for RC-6 final-name regression");
#endif
}

TEST_CASE("RC-6 synthesized RT engine node policy changes the physical plan", "[rendercompiler][rc6a1][rc6-fix-rt]") {
    auto root = rc5sIndependentDocument();
    root["rendering"]["scene_acceleration_structures"] = Rc6Json::array({{{"name", "actors"}}});
    const auto compile = [&] {
        auto resolved = resolveRenderPipeline({root, "rc6-rt"}, {true, RenderPipelineGraphVariant::flat},
            {.normalize_config = [](const auto &config, const auto &) {
                return resolveRenderTargetFormatClassesV2(config, vk::Format::eB8G8R8A8Srgb, {64, 64}, false);
            }});
        auto pipeline = compileRenderPipeline(resolved);
        auto ir = finalizeRenderGraph(parseRenderGraph(resolved.composed_config, {}),
            {.named = pipeline.scene_acceleration_structures, .ray_tracing_project = true}, pipeline.target_planning);
        resolveFinalPlanningPolicy(pipeline, ir);
        std::unordered_map<std::string, FramePlan> frames;
        for (const auto &g : ir.graphs) frames.emplace(g.name, planRenderGraph(ir, g));
        const auto targets = renderTargetDefinitionsOf(ir);
        const auto physical = compileRenderingTargetPlans(ir, frames, targets, pipeline.sample_count_policy,
            vk::Format::eB8G8R8A8Srgb, {.ray_query = true}, {}, {}, pipeline.target_planning);
        CHECK(std::none_of(pipeline.diagnostics.begin(), pipeline.diagnostics.end(), [](const auto &d) {
            return d.kind == RenderPipelineDiagnosticKind::planning_name_absent;
        }));
        return Rc6Json(vulkanTargetPlanToJson(*physical.plans.at(0)));
    };
    const auto automatic = compile();
    REQUIRE(rc6ParallelWith(automatic, "__scene_acceleration_structure"));
    root["rendering"]["target_planning"] = rc6Policy("nodes", "__scene_acceleration_structure", "serial", true, "main");
    const auto plan = compile();
    REQUIRE_FALSE(rc6ParallelWith(plan, "__scene_acceleration_structure"));
    CHECK(rc6Row(plan, "__scene_acceleration_structure", "root").at("status") == "adopted");
}

TEST_CASE("RC-6 B6 removed graph is absent before shared resource and task names", "[rendercompiler][rc6a1][rc6-fix-graph]") {
    auto root = rc5sIndependentDocument();
    root["rendering"]["features"] = {"project://features/local.json"};
    Rc6Json feature{{"schema", "pelican.render_feature"}, {"version", 1}, {"name", "shared"},
        {"buffers", Rc6Json::array({{{"name", "A"}, {"size", 64}}})},
        {"compute_tasks", Rc6Json::array({{{"name", "probe_fill"}, {"shader", "engine://shaders/compute/clustered_light_select"},
            {"writes", {"A"}}, {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}}})}};
    root["rendering"]["target_planning"] = rc6Policy("nodes", "probe_fill", "serial", true, "old");
    root["rendering"]["target_planning"]["graphs"]["old"]["resources"]["A"]["no_alias"] = true;
    // Only the graph goes away. Its shared resource and compute task survive
    // in main, so checking their names alone would incorrectly adopt.
    Rc6Json copy{{"schema", "pelican.render_pipeline"}, {"version", 1}, {"name", "shared_graphs"}, {"config", root["rendering"]}};
    auto policy = root["rendering"]["target_planning"];
    root["rendering"] = {{"features", {"project://features/local.json"}}, {"target_planning", policy}};
    copy["config"].erase("features");
    copy["config"].erase("target_planning");
    copy["config"]["rendering_passes"].push_back({{"name", "old"}, {"passes", Rc6Json::array()}});
    root["rendering"]["pipeline"]["preset"] = "project://with_old.json";
    REQUIRE(copy["config"]["rendering_passes"].back()["name"] == "old");
    copy["config"]["rendering_passes"].erase(copy["config"]["rendering_passes"].end() - 1);
    const auto compiled = rc5CompileWithFacts(root, false, &feature, {{"with_old.json", copy}});
    const auto &ir = *compiled.variants[0].render_graph;
    REQUIRE(std::any_of(ir.buffers.begin(), ir.buffers.end(), [](const auto &r) { return r.name == "A"; }));
    const auto order = framePlanOrder(compiled.variants[0].frame_plans.at("main"));
    REQUIRE(std::find(order.begin(), order.end(), "probe_fill") != order.end());
    const auto &pipeline = *compiled.variants[0].compiled_pipeline;
    const auto g = std::find_if(pipeline.target_planning.graphs.begin(), pipeline.target_planning.graphs.end(), [](const auto &g) { return g.graph == "old"; });
    REQUIRE(g != pipeline.target_planning.graphs.end());
    REQUIRE(g->declarations.size() == 2);
    CHECK(g->nodes.empty()); CHECK(g->resources.empty());
    for (const auto &e : g->declarations) CHECK(e.status == PlanningPolicyStatus::absent);
    CHECK(std::count_if(pipeline.diagnostics.begin(), pipeline.diagnostics.end(), [](const auto &d) {
        return d.kind == RenderPipelineDiagnosticKind::planning_name_absent && d.detail_fields.at("graph") == "old";
    }) == 2);
}

TEST_CASE("RC-6 selected integration owns its resources and passes only", "[rendercompiler][rc6a1][rc6-fix-integration]") {
    auto root = rc5sIndependentDocument();
    root["rendering"]["features"] = {"project://features/local.json", "project://peer.json"};
    Rc6Json feature{{"schema", "pelican.render_feature"}, {"version", 1}, {"name", "provider"}};
    const auto base_pass = root["rendering"]["rendering_passes"][0]["passes"][0];
    auto pass = base_pass; pass["name"] = "integrated_pass"; pass["output"]["color"] = "integrated_image";
    Rc6Json integration{{"name", "selected"}, {"requires", {"peer"}}};
    integration["fragment"]["render_targets"] = Rc6Json::array({{{"name", "integrated_image"},
        {"extent_scale", 1.0}, {"format", "B8G8R8A8_UNORM"}, {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}});
    integration["fragment"]["passes"] = Rc6Json::array({{{"insert", "before:ldr"}, {"pass", pass}}});
    feature["integrations"] = Rc6Json::array({integration});
    Rc6Json peer{{"schema", "pelican.render_feature"}, {"version", 1}, {"name", "peer"}};
    const auto compile = [&] { return rc6Physical(rc5CompileWithFacts(root, false, &feature, {{"peer.json", peer}}), "main"); };
    const auto automatic = compile();
    REQUIRE(rc6ParallelWith(automatic, "integrated_pass"));
    feature["target_planning"] = rc6Policy("nodes", "integrated_pass", "serial", true, "main");
    feature["target_planning"]["graphs"]["main"]["resources"]["integrated_image"]["no_alias"] = true;
    const auto selected = compile();
    REQUIRE_FALSE(rc6ParallelWith(selected, "integrated_pass"));
    CHECK(rc6Row(selected, "integrated_image", "part").at("status") == "adopted");
    CHECK(rc6Row(selected, "integrated_pass", "part").at("status") == "adopted");
    SECTION("unselected feature requirement") {
        root["rendering"]["features"].erase(root["rendering"]["features"].end() - 1);
        CHECK(rc6Refusal([&] { compile(); }) == "target_planning.policy_foreign_name@1");
    }
    SECTION("unselected pass requirement") {
        feature["integrations"][0]["requires_passes"] = {"missing_pass"};
        CHECK(rc6Refusal([&] { compile(); }) == "target_planning.policy_foreign_name@1");
    }
    SECTION("borrowed resource") {
        feature["target_planning"]["graphs"]["main"]["resources"]["independent"]["no_alias"] = true;
        CHECK(rc6Refusal([&] { compile(); }) == "target_planning.policy_foreign_name@1");
    }
}

TEST_CASE("RC-6 typed policy targets carry future string vocabulary without node constraints", "[rendercompiler][rc6a1][rc6-fix-types]") {
    CompiledRenderPipeline pipeline;
    PlanningGraphConstraints graph{.graph = "main"};
    graph.declarations = {
        {.graph="main", .target="main", .key="backend", .value=std::string{"transient"}, .target_kind=PlanningTargetKind::graph},
        {.graph="main", .target="A", .key="representation", .value=std::string{"materialized"}, .target_kind=PlanningTargetKind::resource},
        {.graph="main", .target="A", .key="alias_group", .value=std::string{"shared"}, .target_kind=PlanningTargetKind::resource},
        {.graph="main", .target="gone", .key="representation", .value=std::string{"materialized"}, .target_kind=PlanningTargetKind::resource}};
    pipeline.target_planning.graphs.push_back(graph);
    RenderGraphIR ir;
    ir.images.push_back({.name="A"}); ir.graphs.push_back({.name="main"});
    REQUIRE_NOTHROW(resolveFinalPlanningPolicy(pipeline, ir));
    const auto &resolved = pipeline.target_planning.graphs[0];
    REQUIRE(resolved.declarations.size() == 4);
    CHECK(resolved.declarations[0].status == PlanningPolicyStatus::adopted);
    CHECK(resolved.declarations[1].status == PlanningPolicyStatus::adopted);
    CHECK(resolved.declarations[2].status == PlanningPolicyStatus::adopted);
    CHECK(resolved.declarations[3].status == PlanningPolicyStatus::absent);
    CHECK(resolved.nodes.empty()); CHECK(resolved.resources.empty());
    CHECK(planningPolicyToJson(resolved.declarations)[2]["value"] == "shared");
    const auto parsed = parseTargetPlanningPolicy({{"target_planning", {{"graphs", {{"main", {{"backend", "transient"}}}}}}}}, {});
    REQUIRE(parsed.graphs[0].declarations.size() == 1);
    CHECK(parsed.graphs[0].declarations[0].target_kind == PlanningTargetKind::graph);
    CHECK(std::get<std::string>(parsed.graphs[0].declarations[0].value) == "transient");
}


namespace {
Rc6Json rc6TaskOwner() {
    auto feature = Rc6Json::parse(PathResolver{}.loadText("engine://features/ui.json"));
    feature["name"] = "task_owner";
    feature.erase("passes");
    feature["compute_tasks"] = Rc6Json::array({{{"name", "probe_fill"},
        {"shader", "engine://shaders/compute/clustered_light_select"},
        {"dispatch", {{"groups", {1, 1, 1}}}}, {"schedule", "per_frame"}}});
    return feature;
}
Rc6Json rc6ComputeNode(const char *name) {
    return {{"id", name}, {"op", "render.compute"},
        {"parameters", {{"shader", "engine://shaders/compute/clustered_light_select"},
            {"ports", Rc6Json::object()}, {"dispatch", {{"groups", {1, 1, 1}}}}}},
        {"inputs", Rc6Json::object()}, {"outputs", Rc6Json::array()}};
}
}

TEST_CASE("RC-6 root final task name wins over another task author name", "[rendercompiler][rc6a1][rc6-fix2-final]") {
#if PELICAN_WITH_OPENXR
    auto root = rc5Document(false, false, false);
    root["rendering"]["features"] = {"project://features/local.json"};
    auto feature = rc6TaskOwner();
    auto second = feature["compute_tasks"][0];
    second["name"] = "probe_fill#xr";
    feature["compute_tasks"].push_back(second);
    // Check both variants; the same root spelling intentionally selects a
    // different owner in flat and XR, according to their final names.
    for (const bool xr : {false, true}) {
        CAPTURE(xr);
        const auto *graph = xr ? "main_render#xr" : "main_render";
        const auto *other = xr ? "probe_fill#xr#xr" : "probe_fill";
        root["rendering"].erase("target_planning");
        const auto automatic = rc6Physical(rc5CompileWithFacts(root, xr, &feature), graph);
        REQUIRE(rc6ParallelWith(automatic, "probe_fill#xr"));
        REQUIRE(rc6ParallelWith(automatic, other));
        root["rendering"]["target_planning"] = rc6Policy("nodes", "probe_fill", "serial", true);
        const auto fallback = rc6Physical(rc5CompileWithFacts(root, xr, &feature), graph);
        CHECK_FALSE(rc6ParallelWith(fallback, xr ? "probe_fill#xr" : "probe_fill"));
        CHECK(rc6ParallelWith(fallback, xr ? "probe_fill#xr#xr" : "probe_fill#xr"));
        root["rendering"]["target_planning"] = rc6Policy("nodes", "probe_fill#xr", "serial", true);
        const auto result = rc5CompileWithFacts(root, xr, &feature);
        const auto plan = rc6Physical(result, graph);
        CHECK_FALSE(rc6ParallelWith(plan, "probe_fill#xr"));
        CHECK(rc6ParallelWith(plan, other));
        CHECK(rc6Row(plan, "probe_fill#xr", "root").at("status") == "adopted");
        CHECK(std::none_of(result.variants[0].compiled_pipeline->diagnostics.begin(), result.variants[0].compiled_pipeline->diagnostics.end(), [](const auto &d) {
            return d.kind == RenderPipelineDiagnosticKind::planning_name_absent;
        }));
        // A part continues to use its owned author name despite that spelling
        // also being the final name of the first task in XR.
        feature["target_planning"] = rc6Policy("nodes", "probe_fill#xr", "serial", true);
        const auto part = rc6Physical(rc5CompileWithFacts(root, xr, &feature), graph);
        CHECK(rc6ParallelWith(part, other) == !xr);
        CHECK(rc6Row(part, xr ? "probe_fill#xr#xr" : "probe_fill#xr", "part").at("status") == (xr ? "adopted" : "overridden"));
        feature.erase("target_planning");
    }
#else
    FAIL("XR build required for RC-6 final-name collision regression");
#endif
}

TEST_CASE("RC-6 fold binds the authored task before flat and XR final names collide", "[rendercompiler][rc6a1][rc6-fix3-fold]") {
#if PELICAN_WITH_OPENXR
    auto root = rc5Document(false, false, false);
    root["nodes"] = Rc6Json::array({rc6ComputeNode("probe_fill"), rc6ComputeNode("probe_fill#xr")});
    const std::vector<std::string> subjects{"probe_fill#xr"};
    const auto digest = graphFoldGroupDigest(root, subjects);
    for (const bool xr : {false, true}) {
        CAPTURE(xr);
        const auto *graph = xr ? "main_render#xr" : "main_render";
        const auto *selected = xr ? "probe_fill#xr#xr" : "probe_fill#xr";
        const auto *other = xr ? "probe_fill#xr" : "probe_fill";
        root.erase("fold_groups");
        const auto automatic = rc6Physical(rc5CompileWithFacts(root, xr), graph);
        REQUIRE(rc6ParallelWith(automatic, selected));
        REQUIRE(rc6ParallelWith(automatic, other));
        root["fold_groups"]["probe"] = {{"nodes", subjects}, {"folded", {
            {"kind", "decisions"}, {"decisions", {{"serial", Rc6Json::array({{{"node", "probe_fill#xr"}}})}}},
            {"verified", {{"digest", digest}, {"at", "2026-10-28T00:00:00Z"}}}}}};
        const auto result = rc5CompileWithFacts(root, xr);
        const auto plan = rc6Physical(result, graph);
        CHECK_FALSE(rc6ParallelWith(plan, selected));
        CHECK(rc6ParallelWith(plan, other));
        const auto row = rc6Row(plan, selected, "root");
        CHECK(row.at("status") == "adopted");
        CHECK(row.at("origin").at("pointer") == "/fold_groups/probe/folded/decisions/serial");
        const auto &policy = result.variants[0].compiled_pipeline->target_planning.graphs;
        const auto g = std::find_if(policy.begin(), policy.end(), [&](const auto &g) { return g.graph == graph; });
        REQUIRE(g != policy.end());
        const auto entry = std::find_if(g->declarations.begin(), g->declarations.end(), [&](const auto &e) { return e.target == selected; });
        REQUIRE(entry != g->declarations.end());
        REQUIRE(entry->binding.has_value());
        CHECK(entry->binding->authored_name == "probe_fill#xr");
        CHECK(entry->binding->owner == "task/probe_fill#xr");
    }
#else
    FAIL("XR build required for RC-6 fold collision regression");
#endif
}

TEST_CASE("RC-6 replaced task keeps old final root policy absent", "[rendercompiler][rc6a1][rc6-fix2-replaced]") {
#if PELICAN_WITH_OPENXR
    auto root = rc5Document(false, false, false);
    root["nodes"] = Rc6Json::array({rc6ComputeNode("probe_fill#xr")});
    root["rendering"]["features"] = Rc6Json::array({{{"ref", "project://features/local.json"},
        {"pass_replacements", {{"probe_fill", "probe_fill#xr"}}}}});
    auto feature = rc6TaskOwner();
    auto independent = feature["compute_tasks"][0];
    independent["name"] = "independent";
    feature["compute_tasks"].push_back(independent);
    const auto automatic = rc6Physical(rc5CompileWithFacts(root, true, &feature), "main_render#xr");
    REQUIRE(rc6ParallelWith(automatic, "probe_fill#xr#xr"));
    root["rendering"]["target_planning"] = rc6Policy("nodes", "probe_fill#xr", "serial", true);
    feature["target_planning"] = rc6Policy("nodes", "probe_fill", "serial", true);
    const auto result = rc5CompileWithFacts(root, true, &feature);
    const auto plan = rc6Physical(result, "main_render#xr");
    CHECK(rc6ParallelWith(plan, "probe_fill#xr#xr"));
    CHECK(plan.at("planning_opportunities").at("parallel_candidates") == automatic.at("planning_opportunities").at("parallel_candidates"));
    CHECK(rc6Row(plan, "probe_fill#xr", "root").at("status") == "absent");
    CHECK(rc6Row(plan, "probe_fill#xr", "part").at("status") == "absent");
    CHECK(std::count_if(result.variants[0].compiled_pipeline->diagnostics.begin(), result.variants[0].compiled_pipeline->diagnostics.end(), [](const auto &d) {
        return d.kind == RenderPipelineDiagnosticKind::planning_name_absent &&
            d.detail_fields.at("name") == "probe_fill#xr" && d.detail_fields.at("key") == "serial";
    }) == 1);
#else
    FAIL("XR build required for RC-6 replacement regression");
#endif
}

TEST_CASE("RC-6 ambiguous final policy name reports owners before IR collision", "[rendercompiler][rc6a1][rc6-fix2-ambiguous]") {
#if PELICAN_WITH_OPENXR
    auto root = rc5Document(false, false, false);
    root["nodes"] = Rc6Json::array({fg1Image("probe_image"), fg1Fill("probe_fill#xr", "probe_image")});
    root["rendering"]["features"] = {"project://features/local.json"};
    auto feature = rc6TaskOwner();
    for (const auto *name : {"probe_fill#xr", "probe_fill"}) {
        CAPTURE(name);
        root["nodes"][1]["id"] = name;
        root["rendering"]["target_planning"] = rc6Policy("nodes", name, "serial", true);
        try {
            rc5CompileWithFacts(root, true, &feature);
            FAIL("ambiguous policy name accepted");
        } catch (const GraphRegistryError &e) {
            CHECK(e.code() == "target_planning.policy_name_ambiguous@1");
            CHECK(e.detail().at("name") == name);
            REQUIRE(e.detail().at("candidates").size() == 2);
            CHECK(e.detail().at("candidates")[0].at("owner") != e.detail().at("candidates")[1].at("owner"));
        }
    }
#else
    FAIL("XR build required for RC-6 ambiguity regression");
#endif
}
