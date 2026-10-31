namespace {
using Rc6a2Json = nlohmann::json;
Rc6a2Json rc6a2Policy(const CompiledLogicalRenderGraph &graph, Rc6a2Json entries) {
    return {{"target_planning", {{"graphs", {{graph.name, std::move(entries)}}}}}};
}
VulkanTargetPlan rc6a2Compile(const LogicalTypeRegistry &types, const CompiledLogicalRenderGraph &graph,
                            Rc6a2Json entries = Rc6a2Json::object(), bool tile = true,
                            PlanningProfile profile = {}, std::vector<PlanningPolicyEntry> extra = {},
                            std::vector<VulkanPhysicalAttachmentPlan> attachments = {}) {
    auto parsed = parseTargetPlanningPolicy(rc6a2Policy(graph, std::move(entries)), {},
        {PlanningOriginKind::root, "project://root.json", "/rendering"});
    auto &constraints = parsed.graphs.at(0);
    constraints.declarations.insert(constraints.declarations.end(), extra.begin(), extra.end());
    const auto order = graph.name == "completed_local_read_scope" ? FramePlanNodeOrder{graph, {"A", "B", "C"}}
        : graph.name == "attachment_contract" || graph.name == "independent_versions"
            ? FramePlanNodeOrder{graph, {"A", "B"}} : fixtureOrder(graph);
    auto bindings = bindingsFor(types, graph);
    std::map<std::string, ImageUsage, std::less<>> usage;
    if (graph.name == "alias_lifetimes") {
        for (const auto &r : graph.resources) usage[r.name] = ImageUsageBit::color_attachment | ImageUsageBit::sampled;
        if (attachments.empty()) for (const auto &node : graph.nodes) for (const auto &u : node.uses)
            if (u.output_value && u.intent == LogicalAccessIntent::attachment)
                attachments.push_back({.node = node.name, .logical_resource = u.output_value->resource,
                    .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::store}});
    }
    if (graph.name == "completed_local_read_scope")
        for (auto &binding : bindings)
            binding.extent = ResourceExtentPlan{.kind = ResourceExtentKind::fixed, .width = 128, .height = 128};
    return compileVulkanTargetPlan(types, graph, topology(tile), providers(), {
        .policy = constraints.declarations, .node_order = order, .endpoint = "device:0",
        .provider = std::string{kProvider}, .pattern_bindings = std::move(bindings), .effective_image_usage = std::move(usage),
        .memory_requirements = TestSupport::rc7cMemoryRequirements, .profile = profile,
        .node_constraints = constraints.nodes, .resource_constraints = constraints.resources,
        .automatic_attachments = std::move(attachments)});
}
CompiledLogicalRenderGraph rc6a2AliasGraph(const LogicalTypeRegistry &types, bool overlapping = false) {
    CompiledLogicalRenderGraph graph;
    graph.name = "alias_lifetimes";
    graph.resources = {{.name = "temporary_a", .type = sceneLinearHdrV1(types)},
                       {.name = "temporary_b", .type = sceneLinearHdrV1(types)}};
    LogicalGraphNode wa{.name = "WriteA"}, ra{.name = "ReadA"}, wb{.name = "WriteB"}, rb{.name = "ReadB"};
    addWrite(types, wa, graph.resources[0], 1, "out");
    addRead(types, ra, graph.resources[0], 1, "in", LogicalReadFootprintKind::same_pixel);
    addWrite(types, wb, graph.resources[1], 1, "out");
    addRead(types, rb, graph.resources[1], 1, "in", LogicalReadFootprintKind::same_pixel);
    if (overlapping) addRead(types, rb, graph.resources[0], 1, "also_a", LogicalReadFootprintKind::same_pixel);
    graph.nodes = {wa, ra, wb, rb};
    validateCompiledLogicalRenderGraph(types, graph);
    return graph;
}
Rc6a2Json rc6a2Group(std::string strength = "require") {
    const Rc6a2Json group{{"value", "scratch"}, {"strength", strength}};
    return {{"resources", {{"temporary_a", {{"alias_group", group}}}, {"temporary_b", {{"alias_group", group}}}}}};
}
const PlanningPolicyEntry &rc6a2Entry(const VulkanTargetPlan &plan, std::string_view key) {
    const auto row = std::find_if(plan.policy.begin(), plan.policy.end(), [&](const auto &e) { return e.key == key; });
    REQUIRE(row != plan.policy.end());
    return *row;
}
void rc6a2Refusal(const std::function<void()> &f, std::string_view code, std::string_view key) {
    try { f(); FAIL("policy did not refuse"); }
    catch (const GraphRegistryError &e) {
        CHECK(e.code() == std::string{code});
        CHECK(e.detail().dump().find(key) != std::string::npos);
    }
}
}

TEST_CASE("RC-6a2 C1 five keys change physical results before selection", "[target-render-planning][rc6a2][rc6-c1]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto automatic = rc6a2Compile(types, graph);
    REQUIRE(automatic.backend_selection.selected_candidate == "pelican.vulkan.tile_local_plan@1");
    REQUIRE(physicalResource(automatic, "gbuffer_0").representation == VulkanResourceRepresentation::tile_local_attachment);
    REQUIRE(oneScopeContains(automatic, "GBuffer", "Lighting"));
    SECTION("backend") {
        const auto plan = rc6a2Compile(types, graph, {{"backend", "materialized"}});
        CHECK(plan.backend_selection.selected_candidate == "pelican.vulkan.materialized_plan@1");
        CHECK(rc6a2Entry(plan, "backend").satisfied == true);
    }
    SECTION("representation") {
        const auto scratch = graphWithWriteOnlyAttachment(types);
        const auto scratch_automatic = rc6a2Compile(types, scratch);
        REQUIRE(physicalResource(scratch_automatic, "write_only_scratch").representation == VulkanResourceRepresentation::transient_attachment);
        REQUIRE(scratch_automatic.backend_selection.selected_candidate == "pelican.vulkan.tile_local_plan@1");
        const auto plan = rc6a2Compile(types, scratch, {{"backend", "tile_local"},
            {"resources", {{"write_only_scratch", {{"representation", "materialized"}}}}}});
        const auto dump = vulkanTargetPlanToJson(plan);
        CHECK(physicalResource(plan, "write_only_scratch").representation == VulkanResourceRepresentation::materialized_image);
        CHECK(plan.backend_selection.selected_candidate == scratch_automatic.backend_selection.selected_candidate);
        CHECK(rc6a2Entry(plan, "representation").satisfied == true);
        const auto same = rc6a2Compile(types, graph, {{"resources", {{"display_output", {{"representation", "materialized"}}}}}});
        CHECK(same.automatic_plan_fingerprint == automatic.automatic_plan_fingerprint);
        CHECK(same.logical_graph_fingerprint == automatic.logical_graph_fingerprint);
        CHECK(dump.at("policy").at(0).at("satisfied") == true);
    }
    SECTION("scope_break") {
        const auto plan = rc6a2Compile(types, graph, {{"backend", "tile_local"}, {"nodes", {{"Lighting", {{"scope_break", true}}}}}});
        CHECK_FALSE(oneScopeContains(plan, "GBuffer", "Lighting"));
        CHECK(rc6a2Entry(plan, "scope_break").satisfied == true);
        CHECK(physicalNodeOrderOf(plan.scopes).nodes == fixtureOrder(graph).nodes());
    }
    SECTION("scope_join") {
        const auto desktop = rc6a2Compile(types, graph, Rc6a2Json::object(), false);
        REQUIRE_FALSE(oneScopeContains(desktop, "Lighting", "Forward"));
        const auto plan = rc6a2Compile(types, graph, {{"nodes", {{"Forward", {{"scope_join", true}}}}}}, false);
        CHECK(oneScopeContains(plan, "Lighting", "Forward"));
        CHECK(rc6a2Entry(plan, "scope_join").satisfied == true);
    }
    SECTION("alias_group") {
        const auto alias = rc6a2AliasGraph(types);
        const PlanningProfile debug{PlanningProfileKind::conservative_debug};
        REQUIRE(rc6a2Compile(types, alias, Rc6a2Json::object(), false, debug).alias_groups.empty());
        const auto plan = rc6a2Compile(types, alias, rc6a2Group(), false, debug);
        REQUIRE(plan.alias_groups.size() == 1);
        CHECK(plan.alias_groups[0].resources == std::vector<std::string>{"temporary_a", "temporary_b"});
        CHECK(rc6a2Entry(plan, "alias_group").satisfied == true);
    }
}

TEST_CASE("RC-6a2 C2 scope break materializes crossing data inside the tile candidate", "[target-render-planning][rc6a2][rc6-c2]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto automatic = rc6a2Compile(types, graph);
    REQUIRE(automatic.backend_selection.selected_candidate == "pelican.vulkan.tile_local_plan@1");
    REQUIRE(physicalResource(automatic, "gbuffer_0").representation == VulkanResourceRepresentation::tile_local_attachment);
    REQUIRE(oneScopeContains(automatic, "GBuffer", "Lighting"));
    const auto plan = rc6a2Compile(types, graph, {{"nodes", {{"Lighting", {{"scope_break", true}}}}}});
    CHECK(plan.backend_selection.selected_candidate == automatic.backend_selection.selected_candidate);
    CHECK(physicalResource(plan, "gbuffer_0").representation == VulkanResourceRepresentation::materialized_image);
    CHECK(physicalResource(plan, "scene_color").representation == physicalResource(automatic, "scene_color").representation);
    CHECK_FALSE(oneScopeContains(plan, "GBuffer", "Lighting"));
    CHECK(oneScopeContains(plan, "Lighting", "Forward"));
}

TEST_CASE("RC-6a2 C3 impossible alias and isolated join reject require and explain prefer", "[target-render-planning][rc6a2][rc6-c3]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    SECTION("overlapping aliases") {
        const auto graph = rc6a2AliasGraph(types, true);
        REQUIRE(rc6a2Compile(types, graph, Rc6a2Json::object(), false).alias_groups.empty());
        rc6a2Refusal([&] { rc6a2Compile(types, graph, rc6a2Group(), false); }, "target_planning.requirement_unsatisfied@1", "alias_group");
        const auto plan = rc6a2Compile(types, graph, rc6a2Group("prefer"), false);
        CHECK(plan.alias_groups.empty());
        CHECK(std::any_of(plan.policy.begin(), plan.policy.end(), [](const auto &e) { return e.status == PlanningPolicyStatus::infeasible_prefer; }));
    }
    SECTION("isolate or serial on either side") {
        const auto graph = hybridGraph(types, 3, false);
        REQUIRE(oneScopeContains(rc6a2Compile(types, graph), "Lighting", "Forward"));
        for (const auto *side : {"Lighting", "Forward"}) for (const auto *key : {"isolate", "serial"}) {
            Rc6a2Json policy{{"nodes", {{side, {{key, true}}}}}};
            policy["nodes"]["Forward"]["scope_join"] = true;
            rc6a2Refusal([&] { rc6a2Compile(types, graph, policy); }, "target_planning.requirement_unsatisfied@1", "scope_join");
            policy["nodes"]["Forward"]["scope_join"] = {{"value", true}, {"strength", "prefer"}};
            const auto plan = rc6a2Compile(types, graph, policy);
            CHECK_FALSE(oneScopeContains(plan, "Lighting", "Forward"));
            CHECK(rc6a2Entry(plan, "scope_join").status == PlanningPolicyStatus::infeasible_prefer);
        }
    }
}

TEST_CASE("RC-6a2 C4 final verifier rejects unapplied requirements independently", "[target-render-planning][rc6a2][rc6-c4]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto automatic = rc6a2Compile(types, graph);
    REQUIRE(oneScopeContains(automatic, "GBuffer", "Lighting"));
    REQUIRE(physicalResource(automatic, "gbuffer_0").representation == VulkanResourceRepresentation::tile_local_attachment);
    for (const auto &entries : std::vector<Rc6a2Json>{
             {{"backend", "materialized"}}, {{"resources", {{"gbuffer_0", {{"representation", "materialized"}}}}}},
             {{"nodes", {{"Lighting", {{"scope_break", true}}}}}}, {{"nodes", {{"Lighting", {{"serial", true}}}}}},
             {{"nodes", {{"Lighting", {{"isolate", true}}}}}}}) {
        auto plan = automatic;
        plan.policy = parseTargetPlanningPolicy(rc6a2Policy(graph, entries)).graphs[0].declarations;
        rc6a2Refusal([&] { validateVulkanTargetPlanRequirements(graph, plan); }, "target_planning.requirement_unsatisfied@1", plan.policy[0].key);
        CHECK(plan.policy[0].satisfied == false);
    }
    const auto alias = rc6a2AliasGraph(types);
    auto desktop = rc6a2Compile(types, graph, Rc6a2Json::object(), false);
    REQUIRE_FALSE(oneScopeContains(desktop, "Lighting", "Forward"));
    desktop.policy = parseTargetPlanningPolicy(rc6a2Policy(graph, {{"nodes", {{"Forward", {{"scope_join", true}}}}}})).graphs[0].declarations;
    rc6a2Refusal([&] { validateVulkanTargetPlanRequirements(graph, desktop); }, "target_planning.requirement_unsatisfied@1", "scope_join");
    auto fused = automatic;
    fused.policy = parseTargetPlanningPolicy(rc6a2Policy(graph, {{"nodes", {{"Lighting", {{"fuse", false}}}}}})).graphs[0].declarations;
    REQUIRE_NOTHROW(validateVulkanTargetPlanRequirements(graph, fused));
    for (auto &node : fused.lowering_graph.nodes) if (node.logical.name == "Lighting") node.source_nodes.push_back("Forward");
    rc6a2Refusal([&] { validateVulkanTargetPlanRequirements(graph, fused); }, "target_planning.requirement_unsatisfied@1", "fuse");
    auto plan = rc6a2Compile(types, alias, Rc6a2Json::object(), false);
    REQUIRE_FALSE(plan.alias_groups.empty());
    plan.policy = parseTargetPlanningPolicy(rc6a2Policy(alias, {{"resources", {{"temporary_a", {{"no_alias", true}}}}}})).graphs[0].declarations;
    rc6a2Refusal([&] { validateVulkanTargetPlanRequirements(alias, plan); }, "target_planning.requirement_unsatisfied@1", "no_alias");
    plan = rc6a2Compile(types, alias, Rc6a2Json::object(), false, {PlanningProfileKind::conservative_debug});
    REQUIRE(plan.alias_groups.empty());
    plan.policy = parseTargetPlanningPolicy(rc6a2Policy(alias, rc6a2Group())).graphs[0].declarations;
    rc6a2Refusal([&] { validateVulkanTargetPlanRequirements(alias, plan); }, "target_planning.requirement_unsatisfied@1", "alias_group");
}

TEST_CASE("RC-6a2 C5 alias names belong to their declaring documents", "[target-render-planning][rc6a2][rc6-c5]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = rc6a2AliasGraph(types);
    const PlanningProfile debug{PlanningProfileKind::conservative_debug};
    REQUIRE(rc6a2Compile(types, graph, Rc6a2Json::object(), false, debug).alias_groups.empty());
    std::vector<PlanningPolicyEntry> declarations;
    for (const auto *name : {"temporary_a", "temporary_b"}) {
        auto document = rc6a2Policy(graph, {{"resources", {{name, {{"alias_group", "scratch"}}}}}});
        document["render_targets"] = Rc6a2Json::array({{{"name", name}}});
        const auto source = collectPartPlanningPolicy(document, {PlanningOriginKind::part, std::string{"project://"} + name + ".json", {}});
        const auto &rows = source.policy.graphs[0].declarations;
        declarations.insert(declarations.end(), rows.begin(), rows.end());
    }
    REQUIRE(declarations.size() == 2);
    const auto separate = rc6a2Compile(types, graph, Rc6a2Json::object(), false, debug, declarations);
    CHECK(separate.alias_groups.empty());
    const auto root = rc6a2Compile(types, graph, rc6a2Group(), false, debug);
    REQUIRE(root.alias_groups.size() == 1);
    CHECK(root.alias_groups[0].resources.size() == 2);
}

TEST_CASE("RC-6a2 C6 malformed scope storage and part backend are policy_invalid", "[target-render-planning][rc6a2][rc6-c6]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    auto graph = hybridGraph(types, 3, false);
    REQUIRE(oneScopeContains(rc6a2Compile(types, graph), "GBuffer", "Lighting"));
    for (const auto &node : std::vector<Rc6a2Json>{
             {{"GBuffer", {{"scope_join", true}}}}, {{"Lighting", {{"scope_join", true}, {"scope_break", true}}}}})
        rc6a2Refusal([&] { rc6a2Compile(types, graph, {{"nodes", node}}); }, "target_planning.policy_invalid@1", "scope_join");
    rc6a2Refusal([&] { parseTargetPlanningPolicy(rc6a2Policy(graph, {{"backend", "materialized"}}), {},
        {PlanningOriginKind::part, "project://feature.json", {}}); }, "target_planning.policy_invalid@1", "backend");
    for (auto &r : graph.resources) if (r.name == "display_output") r.materialization = LogicalMaterializationRequirement::external;
    REQUIRE(physicalResource(rc6a2Compile(types, graph), "display_output").representation == VulkanResourceRepresentation::external);
    rc6a2Refusal([&] { rc6a2Compile(types, graph, {{"resources", {{"display_output", {{"representation", "materialized"}}}}}}); },
        "target_planning.policy_invalid@1", "representation");
    auto buffer_types = types;
    const SemanticTypeId buffer_id{"test", "buffer", 1};
    buffer_types.registerSchema({.id = buffer_id, .constructor = LogicalTypeConstructor::buffer});
    graph.resources.push_back({.name = "buffer", .type = buffer_types.canonicalize(buffer_id)});
    REQUIRE_NOTHROW(rc6a2Compile(buffer_types, graph));
    rc6a2Refusal([&] { rc6a2Compile(buffer_types, graph, {{"resources", {{"buffer", {{"representation", "materialized"}}}}}}); },
        "target_planning.policy_invalid@1", "representation");
}

TEST_CASE("RC-6a2 C7 feasible backend prefer outranks the cheaper tile candidate", "[target-render-planning][rc6a2][rc6-c7]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto automatic = rc6a2Compile(types, graph);
    REQUIRE(automatic.backend_selection.selected_candidate == "pelican.vulkan.tile_local_plan@1");
    const auto plan = rc6a2Compile(types, graph, {{"backend", {{"value", "materialized"}, {"strength", "prefer"}}}});
    CHECK(plan.backend_selection.selected_candidate == "pelican.vulkan.materialized_plan@1");
    CHECK(rc6a2Entry(plan, "backend").status == PlanningPolicyStatus::adopted);
    CHECK_FALSE(rc6a2Entry(plan, "backend").satisfied.has_value());
    const auto fallback = rc6a2Compile(types, graph, {{"backend", {{"value", "tile_local"}, {"strength", "prefer"}}}}, false);
    CHECK(fallback.backend_selection.selected_candidate == "pelican.vulkan.materialized_plan@1");
    CHECK(rc6a2Entry(fallback, "backend").status == PlanningPolicyStatus::infeasible_prefer);
}

TEST_CASE("RC-6a2 require keeps feasible candidates by dropping conflicting storage and alias prefers", "[target-render-planning][rc6a2]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto automatic = rc6a2Compile(types, graph);
    REQUIRE(oneScopeContains(automatic, "GBuffer", "Lighting"));
    const auto plan = rc6a2Compile(types, graph, {{"backend", "tile_local"},
        {"nodes", {{"Lighting", {{"scope_join", true}}}}},
        {"resources", {{"gbuffer_0", {{"representation", {{"value", "materialized"}, {"strength", "prefer"}}}}}}}});
    CHECK(oneScopeContains(plan, "GBuffer", "Lighting"));
    CHECK(rc6a2Entry(plan, "representation").status == PlanningPolicyStatus::infeasible_prefer);
    const auto alias = rc6a2AliasGraph(types);
    REQUIRE_FALSE(rc6a2Compile(types, alias, Rc6a2Json::object(), false).alias_groups.empty());
    auto policy = rc6a2Group();
    policy["resources"]["temporary_a"]["no_alias"] = {{"value", true}, {"strength", "prefer"}};
    const auto group = rc6a2Compile(types, alias, policy, false);
    REQUIRE(group.alias_groups.size() == 1);
    CHECK(rc6a2Entry(group, "no_alias").status == PlanningPolicyStatus::infeasible_prefer);
}

TEST_CASE("RC-6a2 fix1 scope policy checks executable attachment contracts", "[target-render-planning][rc6a2][rc6-fix1]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    CompiledLogicalRenderGraph graph;
    graph.name = "attachment_contract";
    graph.resources = {{.name = "RA", .type = sceneLinearHdrV1(types)},
                       {.name = "RB", .type = sceneLinearHdrV1(types)}};
    LogicalGraphNode a{.name = "A"}, b{.name = "B"};
    addWrite(types, a, graph.resources[0], 1, "out");
    addWrite(types, b, graph.resources[1], 1, "out");
    graph.nodes = {a, b};
    validateCompiledLogicalRenderGraph(types, graph);
    std::vector<VulkanPhysicalAttachmentPlan> attachments{
        VulkanPhysicalAttachmentPlan{.node = "A", .logical_resource = "RA", .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::store}},
        VulkanPhysicalAttachmentPlan{.node = "B", .logical_resource = "RB", .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::store}}};
    const auto compile = [&](Rc6a2Json policy) {
        return rc6a2Compile(types, graph, std::move(policy), false, {}, {}, attachments);
    };
    const auto automatic = compile(Rc6a2Json::object());
    REQUIRE(automatic.backend_selection.selected_candidate == "pelican.vulkan.materialized_plan@1");
    REQUIRE_FALSE(oneScopeContains(automatic, "A", "B"));
    auto policy = Rc6a2Json{{"backend", "materialized"}, {"nodes", {{"B", {{"scope_join", true}}}}}};
    rc6a2Refusal([&] { compile(policy); }, "target_planning.requirement_unsatisfied@1", "scope_join");
    policy["nodes"]["B"]["scope_join"] = {{"value", true}, {"strength", "prefer"}};
    const auto fallback = compile(policy);
    CHECK_FALSE(oneScopeContains(fallback, "A", "B"));
    CHECK(rc6a2Entry(fallback, "scope_join").status == PlanningPolicyStatus::infeasible_prefer);

    // Bypass candidate construction: membership alone must not satisfy either
    // relation when another scope has an impossible rendering instance.
    for (const auto *key : {"scope_join", "scope_break"}) {
        auto invalid = automatic;
        invalid.scopes[0].nodes.push_back("B");
        invalid.scopes[0].single_rendering_instance = true;
        invalid.scopes.pop_back();
        invalid.policy = parseTargetPlanningPolicy(rc6a2Policy(graph,
            {{"nodes", {{key == std::string_view{"scope_join"} ? "B" : "A", {{key, true}}}}}})).graphs[0].declarations;
        rc6a2Refusal([&] { validateVulkanTargetPlanRequirements(graph, invalid); }, "target_planning.requirement_unsatisfied@1", key);
        CHECK(invalid.policy[0].satisfied == false);
    }
    // A matching view is legal only when all intermediate operations preserve
    // it. Exercise depth as well as colors with the real attachment input.
    graph.nodes[1].uses.clear();
    graph.nodes[1].ports.clear();
    addReadWrite(types, graph.nodes[1], graph.resources[0], 1, 2, "out");
    validateCompiledLogicalRenderGraph(types, graph);
    attachments[1].logical_resource = "RA";
    attachments[1].operations.load_op = AttachmentLoadOp::load;
    policy["nodes"]["B"]["scope_join"] = true;
    REQUIRE(oneScopeContains(compile(policy), "A", "B"));
    for (const auto aspect : {AttachmentAspect::color, AttachmentAspect::depth}) {
        attachments[0].aspect = attachments[1].aspect = aspect;
        attachments[1].operations.load_op = AttachmentLoadOp::clear;
        rc6a2Refusal([&] { compile(policy); }, "target_planning.requirement_unsatisfied@1", "scope_join");
        attachments[1].operations.load_op = AttachmentLoadOp::load;
        attachments[0].operations.store_op = AttachmentStoreOp::dont_care;
        rc6a2Refusal([&] { compile(policy); }, "target_planning.requirement_unsatisfied@1", "scope_join");
        attachments[0].operations.store_op = AttachmentStoreOp::store;
    }
}

TEST_CASE("RC-6a2 fix2 drops only the causal prefer when require-only baseline is infeasible", "[target-render-planning][rc6a2][rc6-fix2]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = rc6a2AliasGraph(types);
    const auto automatic = rc6a2Compile(types, graph);
    REQUIRE(automatic.backend_selection.selected_candidate == "pelican.vulkan.tile_local_plan@1");
    REQUIRE(physicalResource(automatic, "temporary_a").representation == VulkanResourceRepresentation::tile_local_attachment);
    auto policy = rc6a2Group();
    policy["backend"] = "tile_local";
    rc6a2Refusal([&] { rc6a2Compile(types, graph, policy); }, "target_planning.requirement_unsatisfied@1", "alias_group");
    for (const auto *name : {"temporary_a", "temporary_b"})
        policy["resources"][name]["representation"] = {{"value", "materialized"}, {"strength", "prefer"}};
    policy["resources"]["temporary_a"]["no_alias"] = {{"value", true}, {"strength", "prefer"}};
    const auto plan = rc6a2Compile(types, graph, policy);
    REQUIRE(plan.alias_groups.size() == 1);
    CHECK(plan.alias_groups[0].resources == std::vector<std::string>{"temporary_a", "temporary_b"});
    CHECK(plan.backend_selection.selected_candidate == "pelican.vulkan.tile_local_plan@1");
    for (const auto &e : plan.policy) {
        if (e.key == "representation") CHECK(e.status == PlanningPolicyStatus::adopted);
        if (e.key == "no_alias") CHECK(e.status == PlanningPolicyStatus::infeasible_prefer);
    }
    CHECK(vulkanTargetPlanToJson(plan) == vulkanTargetPlanToJson(rc6a2Compile(types, graph, policy)));
}

TEST_CASE("RC-6a2 fix3 freezing automatic boundaries preserves independent versions", "[target-render-planning][rc6a2][rc6-fix3]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    CompiledLogicalRenderGraph graph;
    graph.name = "independent_versions";
    graph.resources = {{.name = "R", .type = sceneLinearHdrV1(types)}};
    LogicalGraphNode a{.name = "A"}, b{.name = "B"};
    addWrite(types, a, graph.resources[0], 1, "out");
    addWrite(types, b, graph.resources[0], 2, "out");
    graph.nodes = {a, b};
    validateCompiledLogicalRenderGraph(types, graph);
    const auto automatic = rc6a2Compile(types, graph);
    REQUIRE(automatic.scopes.size() == 2);
    REQUIRE(physicalResource(automatic, "R").representation == VulkanResourceRepresentation::transient_attachment);
    Rc6a2Json frozen = Rc6a2Json::object();
    for (std::size_t i = 1; i < automatic.scopes.size(); ++i)
        frozen["nodes"][automatic.scopes[i].nodes.front()]["scope_break"] = true;
    const auto plan = rc6a2Compile(types, graph, frozen);
    CHECK(physicalResource(plan, "R").representation == physicalResource(automatic, "R").representation);
    CHECK(plan.backend_selection.selected_candidate == automatic.backend_selection.selected_candidate);
    CHECK(plan.automatic_plan_fingerprint == automatic.automatic_plan_fingerprint);
}

TEST_CASE("RC-6a2 fix2a physical locality failure drops the causal prefer", "[target-render-planning][rc6a2][rc6-fix2a]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto automatic = rc6a2Compile(types, graph);
    REQUIRE(automatic.backend_selection.selected_candidate == "pelican.vulkan.tile_local_plan@1");
    REQUIRE(oneScopeContains(automatic, "GBuffer", "Lighting"));
    REQUIRE(physicalResource(automatic, "gbuffer_0").representation == VulkanResourceRepresentation::tile_local_attachment);
    for (const auto *key : {"serial", "isolate"}) {
        INFO(key);
        Rc6a2Json policy{{"backend", "tile_local"}, {"nodes", {{"Lighting", {{key, true}}}}}};
        rc6a2Refusal([&] { rc6a2Compile(types, graph, policy); },
            "target_planning.requirement_unsatisfied@1", key);
        policy["nodes"]["Lighting"][key] = {{"value", true}, {"strength", "prefer"}};
        // An unrelated hint must not be discarded to repair locality.
        policy["resources"]["display_output"]["no_alias"] = {{"value", true}, {"strength", "prefer"}};
        const auto plan = rc6a2Compile(types, graph, policy);
        CHECK(plan.backend_selection.selected_candidate == automatic.backend_selection.selected_candidate);
        CHECK(oneScopeContains(plan, "GBuffer", "Lighting"));
        CHECK(rc6a2Entry(plan, key).status == PlanningPolicyStatus::infeasible_prefer);
        CHECK(rc6a2Entry(plan, "no_alias").status == PlanningPolicyStatus::adopted);
        CHECK(rc6a2Entry(plan, "backend").satisfied == true);
        CHECK(vulkanTargetPlanToJson(plan) == vulkanTargetPlanToJson(rc6a2Compile(types, graph, policy)));
    }
}

TEST_CASE("RC-6a2 fix2b attachment legality waits for completed local reads", "[target-render-planning][rc6a2][rc6-fix2b]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    CompiledLogicalRenderGraph graph;
    graph.name = "completed_local_read_scope";
    graph.resources = {{.name = "M", .type = sceneLinearHdrV1(types),
                       .materialization = LogicalMaterializationRequirement::required},
                       {.name = "T", .type = sceneLinearHdrV1(types)}};
    LogicalGraphNode a{.name = "A"}, b{.name = "B"}, c{.name = "C"};
    addWrite(types, a, graph.resources[0], 1, "out");
    addReadWrite(types, b, graph.resources[0], 1, 2, "out");
    addWrite(types, b, graph.resources[1], 1, "tile");
    addReadWrite(types, c, graph.resources[0], 2, 3, "out");
    addRead(types, c, graph.resources[1], 1, "in", LogicalReadFootprintKind::same_pixel);
    graph.nodes = {a, b, c};
    validateCompiledLogicalRenderGraph(types, graph);
    std::vector<VulkanPhysicalAttachmentPlan> attachments{
        {.node = "A", .logical_resource = "M", .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::store}},
        {.node = "B", .logical_resource = "M", .operations = PassAttachmentOperations{AttachmentLoadOp::load, AttachmentStoreOp::store}},
        {.node = "B", .logical_resource = "T", .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::store}},
        {.node = "C", .logical_resource = "M", .operations = PassAttachmentOperations{AttachmentLoadOp::load, AttachmentStoreOp::store}}};
    const auto compile = [&](Rc6a2Json policy) {
        return rc6a2Compile(types, graph, std::move(policy), true, {}, {}, attachments);
    };
    const auto automatic = compile(Rc6a2Json::object());
    REQUIRE(automatic.scopes.size() == 1);
    REQUIRE(automatic.scopes[0].local_reads == std::vector<std::string>{"T"});
    REQUIRE(physicalResource(automatic, "T").representation == VulkanResourceRepresentation::tile_local_attachment);
    Rc6a2Json policy{{"backend", "tile_local"}, {"nodes", {{"B", {{"scope_join", true}}}}}};
    for (const auto *strength : {"require", "prefer"}) {
        policy["nodes"]["B"]["scope_join"] = {{"value", true}, {"strength", strength}};
        const auto plan = compile(policy);
        REQUIRE(plan.scopes.size() == 1);
        CHECK(plan.scopes[0].nodes == std::vector<std::string>{"A", "B", "C"});
        CHECK(plan.scopes[0].local_reads == std::vector<std::string>{"T"});
        CHECK(rc6a2Entry(plan, "scope_join").status == PlanningPolicyStatus::adopted);
        CHECK(plan.backend_selection.selected_candidate == automatic.backend_selection.selected_candidate);
    }
    // A completed A/B prefix becomes illegal when a preferred serial C removes
    // its local read. The physical contract must blame that boundary as well.
    policy["nodes"]["B"]["scope_join"] = true;
    policy["nodes"]["C"]["serial"] = {{"value", true}, {"strength", "prefer"}};
    const auto repaired = compile(policy);
    REQUIRE(repaired.scopes.size() == 1);
    CHECK(repaired.scopes[0].local_reads == std::vector<std::string>{"T"});
    CHECK(rc6a2Entry(repaired, "serial").status == PlanningPolicyStatus::infeasible_prefer);
    CHECK(rc6a2Entry(repaired, "scope_join").satisfied == true);
}
