namespace {
struct Rc7bFixture {
    LogicalTypeRegistry types = makeBuiltinLogicalTypeRegistry();
    CompiledLogicalRenderGraph graph = rc6a2AliasGraph(types);
    VulkanTargetPlanRequest request{.node_order = fixtureOrder(graph)};
    Rc7bFixture() {
        request.memory_requirements = TestSupport::rc7cMemoryRequirements;
        request.node_order = fixtureOrder(graph);
        request.endpoint = "device:0";
        request.provider = std::string{kProvider};
        request.pattern_bindings = bindingsFor(types, graph);
        for (auto &b : request.pattern_bindings) {
            b.pattern.prefer_transient = false;
            b.pattern.allow_tile_local = false;
            b.pattern.require_store = true; // Store does not retain a resource.
            request.effective_image_usage[b.resource] = ImageUsageBit::color_attachment | ImageUsageBit::sampled | ImageUsageBit::transfer_src;
        }
        for (const auto &n : graph.nodes) for (const auto &u : n.uses)
            if (u.output_value) request.automatic_attachments.push_back({.node = n.name, .logical_resource = u.output_value->resource,
                .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::store}});
    }
    VulkanTargetPlan run() const {
        return compileVulkanTargetPlan(types, graph, topology(false), providers(), request);
    }
    void policy(nlohmann::json entries) {
        auto parsed = parseTargetPlanningPolicy(rc6a2Policy(graph, entries));
        request.policy = parsed.graphs[0].declarations;
        request.node_constraints = parsed.graphs[0].nodes;
        request.resource_constraints = parsed.graphs[0].resources;
    }
    void verify(const VulkanTargetPlan &plan, VulkanCompletePhysicalPlanPackage package) const {
        (void)verifyVulkanCompletePhysicalPlanPackage(graph, topology(false), plan, request.node_order, std::move(package));
    }
};
}

TEST_CASE("RC-7b B1 B6 identical effective image contracts admit depth transfer and BGRA", "[target-render-planning][rc7b][rc7b-b1][rc7b-b6]") {
    for (const auto format : {ImageFormat::d32_sfloat, ImageFormat::b8g8r8a8_unorm, ImageFormat::r8g8b8a8_unorm}) {
        CAPTURE(std::string{imageFormatName(format)});
        Rc7bFixture f;
        for (auto &b : f.request.pattern_bindings) b.pattern.format_candidates = {{.format = format}};
        if (format == ImageFormat::d32_sfloat) {
            for (auto &a : f.request.automatic_attachments) a.aspect = AttachmentAspect::depth;
            for (auto &[name, usage] : f.request.effective_image_usage) usage = ImageUsageBit::depth_stencil_attachment | ImageUsageBit::sampled;
        }
        const auto automatic = f.run();
        REQUIRE(automatic.alias_groups.size() == 1);
        for (const auto &r : automatic.resources) {
            REQUIRE(r.aliasable);
            REQUIRE(r.lifetime.retain.empty());
            REQUIRE(r.rasterization_samples == 1);
            REQUIRE_FALSE(r.resolve_required);
        }
        REQUIRE_NOTHROW(f.verify(automatic, ejectVulkanCompletePhysicalPlanPackage(automatic)));
        SECTION("no_alias") { f.policy({{"resources", {{"temporary_b", {{"no_alias", true}}}}}}); CHECK(f.run().alias_groups.empty()); }
        SECTION("isolate") { f.policy({{"nodes", {{"WriteB", {{"isolate", true}}}}}}); CHECK(f.run().alias_groups.empty()); }
        SECTION("require") { f.policy(rc6a2Group()); CHECK(f.run().alias_groups.size() == 1); }
        SECTION("prefer") { f.policy(rc6a2Group("prefer")); CHECK(f.run().alias_groups.size() == 1); }
    }
}

TEST_CASE("RC-7b first-use controls reach the planner and independent verifier", "[target-render-planning][rc7b][rc7b-controls]") {
    const std::string change = GENERATE("load", "depth_stencil", "mip", "layer", "input", "local_read", "shader_read", "compute", "transfer");
        CAPTURE(change);
        Rc7bFixture f;
        REQUIRE(f.run().alias_groups.size() == 1);
        if (change == "load") f.request.automatic_attachments[1].operations.load_op = AttachmentLoadOp::load;
        if (change == "depth_stencil") {
            for (auto &b : f.request.pattern_bindings) b.pattern.format_candidates = {{.format = ImageFormat::d24_unorm_s8_uint}};
            for (auto &a : f.request.automatic_attachments) a.aspect = AttachmentAspect::depth;
        }
        if (change == "mip") for (auto &b : f.request.pattern_bindings) b.mip_levels.count = 2;
        if (change == "layer") for (auto &b : f.request.pattern_bindings) b.array_layers = 2;
        if (change == "load") {
            auto &n = f.graph.nodes[2];
            n.uses[0] = makeLogicalReadWriteUse("out", {"temporary_b", 0}, {"temporary_b", 1},
                {LogicalReadFootprintKind::same_pixel, std::nullopt}, LogicalAccessIntent::attachment);
            n.ports[0].direction = LogicalPortDirection::input_output;
            f.graph.imports.push_back({.value = {"temporary_b", 0}});
        }
        if (change == "input" || change == "local_read" || change == "shader_read") {
            auto &n = f.graph.nodes[2];
            addRead(f.types, n, f.graph.resources[1], 0, "local",
                change == "shader_read" ? LogicalReadFootprintKind::arbitrary : LogicalReadFootprintKind::same_pixel,
                change == "input" ? LogicalAccessIntent::sampled : LogicalAccessIntent::attachment);
            f.graph.imports.push_back({.value = {"temporary_b", 0}});
        }
        if (change == "compute" || change == "transfer") {
            f.graph.nodes[2].kind = change == "compute" ? LogicalGraphNodeKind::compute : LogicalGraphNodeKind::snapshot_copy;
            f.graph.nodes[2].uses[0].intent = change == "compute" ? LogicalAccessIntent::storage : LogicalAccessIntent::transfer;
            f.request.automatic_attachments.erase(f.request.automatic_attachments.begin() + 1);
        }
        f.request.node_order = fixtureOrder(f.graph);
        const auto rejected = f.run();
        CHECK(rejected.alias_groups.empty());
        CHECK(std::any_of(rejected.decisions.begin(), rejected.decisions.end(), [](const auto &d) {
            return d.id == "pelican.plan.alias_ineligible@1" && d.selected == "first_use";
        }));
        auto package = ejectVulkanCompletePhysicalPlanPackage(rejected);
        for (auto &r : package.resources) r.aliasable = true;
        package.alias_groups = {{"forged", {"temporary_a", "temporary_b"}}};
        CHECK_THROWS_WITH(f.verify(rejected, package), Catch::Matchers::ContainsSubstring("alias.first_use@1"));
        f.policy(rc6a2Group());
        rc6a2Refusal([&] { f.run(); }, "target_planning.requirement_unsatisfied@1", "alias_group");
        CHECK_THROWS_WITH(f.run(), Catch::Matchers::ContainsSubstring("alias.first_use@1"));
}

TEST_CASE("RC-7b retained second resident cannot alias even after the first resident dies", "[target-render-planning][rc7b][rc7b-controls]") {
    for (const std::string reason : {"present", "history", "persistent", "external_depth_export", "xr_mirror", "picking", "capture", "readback"}) {
        CAPTURE(reason);
        Rc7bFixture f;
        const auto legal = f.run();
        REQUIRE(legal.alias_groups.size() == 1);
        REQUIRE(physicalResource(legal, "temporary_a").lifetime.last_use < physicalResource(legal, "temporary_b").lifetime.first_use);
        f.request.retain_reasons["temporary_b"] = {reason};
        const auto retained = f.run();
        CHECK(retained.alias_groups.empty());
        auto package = ejectVulkanCompletePhysicalPlanPackage(retained);
        package.alias_groups = {{"forged", {"temporary_a", "temporary_b"}}};
        for (auto &r : package.resources) r.aliasable = true;
        CHECK_THROWS_WITH(f.verify(retained, package), Catch::Matchers::ContainsSubstring("alias.retained@1"));
    }
}

TEST_CASE("RC-7b effective usage mismatches and unknown facts are unshared", "[target-render-planning][rc7b][rc7b-controls]") {
    Rc7bFixture f;
    const auto before = f.run();
    REQUIRE(before.alias_groups.size() == 1);
    SECTION("different") { f.request.effective_image_usage["temporary_b"] |= ImageUsageBit::storage; SUCCEED(); }
    SECTION("unknown") { f.request.effective_image_usage.clear(); SUCCEED(); }
    const auto after = f.run();
    CHECK(after.alias_groups.empty());
    auto package = ejectVulkanCompletePhysicalPlanPackage(after);
    package.alias_groups = before.alias_groups;
    CHECK_THROWS_WITH(f.verify(after, package), Catch::Matchers::ContainsSubstring("memory.same_offset@1"));
}

TEST_CASE("RC-7b planner excludes provider manual NativeScope boundary facts", "[target-render-planning][rc7b][rc7b-native-control]") {
    Rc7bFixture f;
    REQUIRE(f.run().alias_groups.size() == 1);
    f.request.manual_native_resources = {"temporary_b"};
    const auto plan = f.run();
    CHECK(plan.alias_groups.empty());
    f.policy(rc6a2Group());
    CHECK_THROWS_WITH(f.run(), Catch::Matchers::ContainsSubstring("alias.native_boundary@1"));
    auto package = ejectVulkanCompletePhysicalPlanPackage(plan);
    for (auto &r : package.resources) r.aliasable = true;
    package.alias_groups = {{"forged", {"temporary_a", "temporary_b"}}};
    CHECK_THROWS_WITH(f.verify(plan, package), Catch::Matchers::ContainsSubstring("alias.native_boundary@1"));
}

TEST_CASE("RC-7b verifier rejects native boundaries samples and overlapping residents", "[target-render-planning][rc7b][rc7b-b6]") {
    Rc7bFixture f;
    const auto plan = f.run();
    REQUIRE(plan.alias_groups.size() == 1);
    SECTION("manual") {
        auto package = ejectVulkanCompletePhysicalPlanPackage(plan);
        package.native_scopes = {{.scope = package.scopes.front().id, .implementation = "test.native@1",
            .synchronization = VulkanNativeScopeSynchronizationMode::manual,
            .resources = {{.logical_resource = "temporary_a", .semantic_type = semanticTypeIdName(f.graph.resources[0].type.semantic),
                .access = LogicalAccessMode::write, .stages = {"VK_PIPELINE_STAGE_ALL_COMMANDS_BIT"}, .accesses = {"VK_ACCESS_MEMORY_WRITE_BIT"}}}}};
        auto unshared = package; unshared.alias_groups.clear();
        CHECK_THROWS_WITH(f.verify(plan, unshared), Catch::Matchers::ContainsSubstring("alias.native_boundary@1")); // Residents still share the region.
        CHECK_THROWS_WITH(f.verify(plan, package), Catch::Matchers::ContainsSubstring("alias.native_boundary@1"));
    }
    SECTION("unchecked") {
        auto package = ejectVulkanCompletePhysicalPlanPackage(plan);
        package.native_scopes = {{.scope = package.scopes.front().id, .implementation = "test.native@1",
            .synchronization = VulkanNativeScopeSynchronizationMode::unchecked,
            .resources = {{.logical_resource = "temporary_a", .semantic_type = semanticTypeIdName(f.graph.resources[0].type.semantic),
                .access = LogicalAccessMode::write}}}};
        auto unshared = package; unshared.alias_groups.clear();
        CHECK_THROWS_WITH(f.verify(plan, unshared), Catch::Matchers::ContainsSubstring("alias.native_boundary@1")); // Residents still share the region.
        CHECK_THROWS_WITH(f.verify(plan, package), Catch::Matchers::ContainsSubstring("alias.native_boundary@1"));
    }
    SECTION("resolve") {
        auto package = ejectVulkanCompletePhysicalPlanPackage(plan);
        package.resources.front().resolve_required = true;
        CHECK_THROWS_WITH(f.verify(plan, package), Catch::Matchers::ContainsSubstring("alias.ownership_or_samples@1"));
    }
    SECTION("MSAA") {
        auto package = ejectVulkanCompletePhysicalPlanPackage(plan);
        for (auto &r : package.resources) r.rasterization_samples = 2;
        for (auto &s : package.scopes) s.rasterization_samples = 2;
        CHECK_THROWS_WITH(f.verify(plan, package), Catch::Matchers::ContainsSubstring("alias.ownership_or_samples@1"));
    }
    SECTION("overlap") {
        addRead(f.types, f.graph.nodes.back(), f.graph.resources[0], 1, "still_a", LogicalReadFootprintKind::same_pixel);
        f.request.node_order = fixtureOrder(f.graph);
        const auto overlapping = f.run();
        REQUIRE(overlapping.alias_groups.empty());
        auto package = ejectVulkanCompletePhysicalPlanPackage(overlapping);
        package.alias_groups = plan.alias_groups;
        CHECK_THROWS_WITH(f.verify(overlapping, package), Catch::Matchers::ContainsSubstring("alias.shape_or_lifetime@1"));
    }
    SECTION("same logical scope") {
        auto package = ejectVulkanCompletePhysicalPlanPackage(plan);
        package.scopes[0].nodes = physicalNodeOrderOf(package.scopes).nodes;
        package.scopes[0].single_rendering_instance = false;
        package.scopes.resize(1);
        CHECK_THROWS_WITH(f.verify(plan, package), Catch::Matchers::ContainsSubstring("alias.same_scope@1"));
    }
}

TEST_CASE("RC-7b retain-free variants resolve merged contracts before planner policy", "[target-render-planning][rc7b][rc7b-b6][generation-resources]") {
    for (const bool storage : {false, true}) {
        CAPTURE(storage);
        Rc7bFixture f;
        const auto before = f.run();
        REQUIRE(before.alias_groups.size() == 1);
        RenderGraphIR first, second;
        for (const auto &name : {"temporary_a", "temporary_b"}) first.images.push_back(ImageResource{
            .name = name, .format = ImageFormat::r8g8b8a8_unorm,
            .usage = ImageUsageBit::color_attachment | ImageUsageBit::sampled | ImageUsageBit::transfer_src});
        second = first;
        if (!storage) second.images[1].usage |= ImageUsageBit::storage;
        std::vector<ImagePlacement> placements;
        for (const auto &i : first.images) placements.push_back({.resource = i.name, .alias_group = "runtime/a/b"});
        auto other = placements;
        if (storage) { other[1].storage = RenderTargetStorageMode::transient_attachment; other[1].alias_group.reset(); }
        const std::array variants{GenerationResourceVariant{.name = "flat", .render_graph = &first, .placements = placements},
            GenerationResourceVariant{.name = "xr", .render_graph = &second, .placements = other}};
        const auto generation = resolveGenerationResources(variants, {64, 64});
        REQUIRE_FALSE(generation.alias_rejections.empty());
        for (const auto &image : generation.images) {
            REQUIRE_FALSE(image.alias_group);
            f.request.effective_image_usage[image.declaration.name] = image.usage;
        }
        f.request.generation_alias_exclusions = generation.alias_rejections;
        REQUIRE(f.request.retain_reasons.empty());
        REQUIRE(f.request.policy.empty());
        const auto automatic = f.run();
        CHECK(automatic.alias_groups.empty());
        CHECK(std::any_of(automatic.decisions.begin(), automatic.decisions.end(), [](const auto &d) {
            return d.id == "pelican.plan.alias_ineligible@1" && d.selected.starts_with("generation_contract:");
        }));
        f.policy(rc6a2Group("prefer"));
        const auto preferred = f.run();
        CHECK(preferred.alias_groups.empty());
        CHECK(std::any_of(preferred.policy.begin(), preferred.policy.end(), [](const auto &p) {
            return p.key == "alias_group" && p.status == PlanningPolicyStatus::infeasible_prefer;
        }));
        f.policy(rc6a2Group());
        rc6a2Refusal([&] { f.run(); }, "target_planning.alias_generation_contract@1", "alias_group");
    }
}

TEST_CASE("RC-7b planner excludes nonoverlapping residents within an adopted local-read scope", "[target-render-planning][rc7b][rc7b-scope-control]") {
    Rc7bFixture f;
    for (const auto i : {1, 3}) { f.graph.nodes[i].uses.clear(); f.graph.nodes[i].ports.clear(); }
    f.graph.resources.push_back({.name = "M", .type = sceneLinearHdrV1(f.types), .materialization = LogicalMaterializationRequirement::required});
    f.graph.resources.push_back({.name = "T", .type = sceneLinearHdrV1(f.types)});
    const auto m = f.graph.resources[2], t = f.graph.resources[3];
    addWrite(f.types, f.graph.nodes[0], m, 1, "m");
    addWrite(f.types, f.graph.nodes[0], t, 1, "t");
    for (std::size_t i = 1; i < f.graph.nodes.size(); ++i) {
        addReadWrite(f.types, f.graph.nodes[i], m, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i + 1), "m");
        addRead(f.types, f.graph.nodes[i], t, 1, "t", LogicalReadFootprintKind::same_pixel);
    }
    f.request.pattern_bindings = bindingsFor(f.types, f.graph);
    for (auto &b : f.request.pattern_bindings) {
        b.extent = ResourceExtentPlan{.kind = ResourceExtentKind::fixed, .width = 64, .height = 64};
        if (b.resource != "T") { b.pattern.allow_tile_local = false; b.pattern.prefer_transient = false; }
        if (b.resource == "M") b.pattern.allow_alias = false;
        f.request.effective_image_usage[b.resource] = ImageUsageBit::color_attachment | ImageUsageBit::sampled;
    }
    for (std::size_t i = 0; i < f.graph.nodes.size(); ++i)
        f.request.automatic_attachments.push_back({.node = f.graph.nodes[i].name, .logical_resource = "M",
            .operations = PassAttachmentOperations{i == 0 ? AttachmentLoadOp::clear : AttachmentLoadOp::load, AttachmentStoreOp::store}});
    f.request.automatic_attachments.push_back({.node = "WriteA", .logical_resource = "T",
        .operations = PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::dont_care}});
    f.request.node_order = fixtureOrder(f.graph);
    const auto run = [&] { return compileVulkanTargetPlan(f.types, f.graph, topology(true), providers(), f.request); };
    const auto plan = run();
    REQUIRE(plan.scopes.size() == 1);
    REQUIRE_FALSE(plan.scopes[0].local_reads.empty());
    REQUIRE(physicalResource(plan, "temporary_a").aliasable);
    REQUIRE(physicalResource(plan, "temporary_b").aliasable);
    REQUIRE(physicalResource(plan, "temporary_a").lifetime.last_use < physicalResource(plan, "temporary_b").lifetime.first_use);
    CHECK(plan.alias_groups.empty());
    auto policy = rc6a2Group(); policy["backend"] = "tile_local";
    f.policy(policy);
    rc6a2Refusal([&] { run(); }, "target_planning.requirement_unsatisfied@1", "alias_group");
}
