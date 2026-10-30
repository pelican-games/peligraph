// Included after the RC-6a2 fixture helpers.
namespace {
VulkanTargetPlan rc7aScratch(bool retained, ImageUsage usage = ImageUsageBit::transfer_src,
                           PlanningStrength strength = PlanningStrength::require,
                           TargetResourceRetainReasons reasons = {}) {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = graphWithWriteOnlyAttachment(types);
    const auto parsed = parseTargetPlanningPolicy(rc6a2Policy(graph,
        {{"backend", "tile_local"}, {"resources", {{"write_only_scratch", {{"retain",
            {{"value", retained}, {"strength", strength == PlanningStrength::require ? "require" : "prefer"}}}}}}}}));
    auto attachments = std::vector<VulkanPhysicalAttachmentPlan>{
        {.node = "Scratch", .logical_resource = "write_only_scratch", .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::dont_care}}};
    return compileVulkanTargetPlan(types, graph, topology(true), providers(), {
        .policy = parsed.graphs[0].declarations, .node_order = fixtureOrder(graph),
        .endpoint = "device:0", .provider = std::string{kProvider}, .pattern_bindings = bindingsFor(types, graph),
        .retain_reasons = std::move(reasons), .effective_image_usage = {{"write_only_scratch", usage}},
        .automatic_attachments = std::move(attachments)});
}
}
TEST_CASE("RC-7a F2 retain derives materialization store and terminal lifetime", "[target-render-planning][rc7a][rc7-f2]") {
    const auto automatic = rc7aScratch(false);
    const auto &before = physicalResource(automatic, "write_only_scratch");
    REQUIRE(before.representation == VulkanResourceRepresentation::transient_attachment);
    REQUIRE(before.lifetime.last_use < fixtureOrder(graphWithWriteOnlyAttachment(makeBuiltinLogicalTypeRegistry())).nodes().size() - 1);
    REQUIRE(before.lifetime.retain.empty());
    REQUIRE(automatic.attachments[0].operations.store_op == AttachmentStoreOp::dont_care);
    REQUIRE(automatic.effective_image_usage.at("write_only_scratch") == ImageUsage{ImageUsageBit::transfer_src});
    const auto plan = rc7aScratch(true, ImageUsageBit::transfer_src, PlanningStrength::prefer);
    const auto &r = physicalResource(plan, "write_only_scratch");
    CHECK(r.representation == VulkanResourceRepresentation::materialized_image);
    CHECK(r.lifetime.last_use == plan.opportunities.node_order.size() - 1);
    CHECK(r.lifetime.retain == std::vector<std::string>{"readback"});
    CHECK(plan.attachments[0].operations.store_op == AttachmentStoreOp::store);
    const auto dump = vulkanTargetPlanToJson(plan);
    CHECK(dump.at("attachments")[0].at("store_op") == "store");
    const auto physical = std::find_if(dump.at("resources").begin(), dump.at("resources").end(),
        [](const auto &v) { return v.at("logical_resource") == "write_only_scratch"; });
    REQUIRE(physical != dump.at("resources").end());
    CHECK(physical->at("representation") == "materialized_image");
    CHECK(physical->at("lifetime").at("retain") == nlohmann::json::array({"readback"}));
}
TEST_CASE("RC-7a F2 final verifier independently checks retain contracts", "[target-render-planning][rc7a][rc7-f2]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = graphWithWriteOnlyAttachment(types);
    auto plan = rc7aScratch(true);
    const auto verify = [&] {
    rc6a2Refusal([&] { validateVulkanTargetPlanRequirements(graph, plan); }, "target_planning.requirement_unsatisfied@1", "retain");
    };
    SECTION("ops") { plan.attachments[0].operations.store_op = AttachmentStoreOp::dont_care; verify(); }
    SECTION("missing writer") { plan.attachments.clear(); verify(); }
    SECTION("usage") { plan.effective_image_usage.clear(); verify(); }
    SECTION("lifetime") { for (auto &r : plan.resources) if (r.logical_resource == "write_only_scratch") r.lifetime.last_use = 0; verify(); }
    SECTION("reason") { for (auto &r : plan.resources) if (r.logical_resource == "write_only_scratch") r.lifetime.retain.clear(); verify(); }
    SECTION("representation") { for (auto &r : plan.resources) if (r.logical_resource == "write_only_scratch") r.representation = VulkanResourceRepresentation::transient_attachment; verify(); }
}
TEST_CASE("RC-7a F2 unsupported usage rejects require and discards prefer without residue", "[target-render-planning][rc7a][rc7-f2]") {
    rc6a2Refusal([&] { rc7aScratch(true, ImageUsageBit::color_attachment); }, "target_planning.retain_unsatisfiable@1", "write_only_scratch");
    const auto plan = rc7aScratch(true, ImageUsageBit::color_attachment, PlanningStrength::prefer);
    CHECK(rc6a2Entry(plan, "retain").status == PlanningPolicyStatus::infeasible_prefer);
    CHECK(physicalResource(plan, "write_only_scratch").representation == VulkanResourceRepresentation::transient_attachment);
    CHECK(physicalResource(plan, "write_only_scratch").lifetime.retain.empty());
    CHECK(plan.attachments[0].operations.store_op == AttachmentStoreOp::dont_care);
}
TEST_CASE("RC-7a F3 false preserves independent engine retention", "[target-render-planning][rc7a][rc7-f3]") {
    const auto plan = rc7aScratch(false, ImageUsageBit::transfer_src, PlanningStrength::require,
        {{"write_only_scratch", {"history", "persistent"}}});
    CHECK(physicalResource(plan, "write_only_scratch").lifetime.retain == std::vector<std::string>{"history", "persistent"});
    CHECK(physicalResource(plan, "write_only_scratch").lifetime.last_use == plan.opportunities.node_order.size() - 1);
}

TEST_CASE("RC-7a F1 readback permission belongs to the selected program plan", "[target-render-planning][rc7a][rc7-f1]") {
    const auto retained = rc7aScratch(true);
    const auto automatic = rc7aScratch(false);
    REQUIRE_NOTHROW(validateTargetReadbackRetention(&retained, "write_only_scratch", "retained"));
    rc6a2Refusal([&] { validateTargetReadbackRetention(&automatic, "write_only_scratch", "other_variant"); },
        "render_target.readback_not_retained@1", "retain: true");
    rc6a2Refusal([&] { validateTargetReadbackRetention(nullptr, "write_only_scratch", "missing_plan"); },
        "render_target.readback_not_retained@1", "missing_plan");
    rc6a2Refusal([&] { validateTargetReadbackRetention(&retained, "absent", "retained"); },
        "render_target.readback_not_retained@1", "absent");
    for (const auto *reason : {"persistent", "picking", "history", "xr_mirror", "external_depth_export"}) {
        auto engine = rc7aScratch(false, ImageUsageBit::transfer_src, PlanningStrength::require,
            {{"write_only_scratch", {reason}}});
        rc6a2Refusal([&] { validateTargetReadbackRetention(&engine, "write_only_scratch", reason); },
            "render_target.readback_not_retained@1", reason);
    }
    for (const auto *reason : {"present", "capture"}) {
        const auto final = rc7aScratch(false, ImageUsageBit::transfer_src, PlanningStrength::require,
            {{"write_only_scratch", {reason}}});
        REQUIRE_NOTHROW(validateTargetReadbackRetention(&final, "write_only_scratch", "final"));
    }
}
TEST_CASE("RC-7a F2 every mip and layer writer stores retained contents", "[target-render-planning][rc7a][rc7-f2]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    auto graph = graphWithWriteOnlyAttachment(types);
    LogicalGraphNode second{.name = "SecondScratch"};
    addWrite(types, second, resource(graph, "write_only_scratch"), 2, "out");
    auto order = fixtureOrder(graph).nodes();
    graph.nodes.push_back(second);
    order.insert(order.end() - 1, "SecondScratch");
    auto bindings = bindingsFor(types, graph);
    for (auto &binding : bindings) if (binding.resource == "write_only_scratch") {
        binding.mip_levels.count = 2;
        binding.array_layers = 2;
        binding.extent = ResourceExtentPlan{.kind = ResourceExtentKind::fixed, .width = 16, .height = 16};
    }
    const auto policy = parseTargetPlanningPolicy(rc6a2Policy(graph, {{"resources", {{"write_only_scratch", {{"retain", true}}}}}}));
    const auto plan = compileVulkanTargetPlan(types, graph, topology(false), providers(), {
        .policy = policy.graphs[0].declarations, .node_order = FramePlanNodeOrder{graph, order},
        .endpoint = "device:0", .provider = std::string{kProvider}, .pattern_bindings = bindings,
        .effective_image_usage = {{"write_only_scratch", ImageUsageBit::transfer_src}},
        .automatic_attachments = {
            {.node = "Scratch", .logical_resource = "write_only_scratch", .subresource = ImageSubresourceRange{},
                .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::dont_care}},
            {.node = "SecondScratch", .logical_resource = "write_only_scratch",
                .subresource = ImageSubresourceRange{.base_mip_level = 1, .base_array_layer = 1},
                .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::dont_care}}}});
    REQUIRE(plan.attachments.size() == 2);
    for (const auto &a : plan.attachments) CHECK(a.operations.store_op == AttachmentStoreOp::store);
    CHECK(plan.attachments[1].subresource->base_mip_level == 1);
    CHECK(plan.attachments[1].subresource->base_array_layer == 1);
}
TEST_CASE("RC-7a F2 causal alias retries erase dropped retain lifetime and operations", "[target-render-planning][rc7a][rc7-f2]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = rc6a2AliasGraph(types);
    auto entries = rc6a2Group();
    entries["resources"]["temporary_a"]["retain"] = {{"value", true}, {"strength", "prefer"}};
    const auto policy = parseTargetPlanningPolicy(rc6a2Policy(graph, entries));
    const auto plan = compileVulkanTargetPlan(types, graph, topology(false), providers(), {
        .policy = policy.graphs[0].declarations, .node_order = fixtureOrder(graph), .endpoint = "device:0",
        .provider = std::string{kProvider}, .pattern_bindings = bindingsFor(types, graph),
        .effective_image_usage = {{"temporary_a", ImageUsageBit::transfer_src}, {"temporary_b", ImageUsageBit::transfer_src}},
        .automatic_attachments = {{.node = "WriteA", .logical_resource = "temporary_a",
            .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::dont_care}},
            {.node = "WriteB", .logical_resource = "temporary_b",
            .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::dont_care}}}});
    CHECK(rc6a2Entry(plan, "retain").status == PlanningPolicyStatus::infeasible_prefer);
    CHECK(physicalResource(plan, "temporary_a").lifetime.retain.empty());
    CHECK(physicalResource(plan, "temporary_a").lifetime.last_use == 1);
    CHECK(plan.attachments[0].operations.store_op == AttachmentStoreOp::dont_care);
    REQUIRE(plan.alias_groups.size() == 1);
    CHECK(plan.alias_groups[0].resources == std::vector<std::string>{"temporary_a", "temporary_b"});
}

TEST_CASE("RC-7a fix final verifier covers ToneMap attachment writers", "[target-render-planning][rc7a][rc7a-fix]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = hybridGraph(types, 3, false);
    const auto policy = parseTargetPlanningPolicy(rc6a2Policy(graph,
        {{"resources", {{"display_output", {{"retain", true}}}}}}));
    auto plan = compileVulkanTargetPlan(types, graph, topology(false), providers(), {
        .policy = policy.graphs[0].declarations, .node_order = fixtureOrder(graph),
        .endpoint = "device:0", .provider = std::string{kProvider}, .pattern_bindings = bindingsFor(types, graph),
        .effective_image_usage = {{"display_output", ImageUsageBit::transfer_src}},
        .automatic_attachments = {{.node = "ToneMap", .logical_resource = "display_output",
            .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::dont_care}}}});
    REQUIRE_NOTHROW(validateVulkanTargetPlanRequirements(graph, plan));
    REQUIRE(plan.attachments.size() == 1);
    REQUIRE(plan.attachments[0].operations.store_op == AttachmentStoreOp::store);
    const auto verify = [&] {
        rc6a2Refusal([&] { validateVulkanTargetPlanRequirements(graph, plan); },
            "target_planning.requirement_unsatisfied@1", "retain");
    };
    SECTION("missing row") { plan.attachments.clear(); verify(); }
    SECTION("discarded contents") { plan.attachments[0].operations.store_op = AttachmentStoreOp::dont_care; verify(); }
}
