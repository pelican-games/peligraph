// Included in rendercompilerprogram_test.cpp: use its production input fixture.
namespace {
using Rc5Json = nlohmann::json;

Rc5Json rc5Document(bool lit, bool hdr, bool extras) {
    auto root = TestSupport::renderGraphRoot(Rc5Json{
        {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}});
    auto features = Rc5Json::array();
    if (lit) {
        features.push_back("engine://features/shadow_directional.json");
        features.push_back("engine://features/sky_ambient.json");
    }
    if (hdr) features.push_back("engine://features/hdr.json");
    if (extras) {
        features.push_back("project://features/local.json");
        root["nodes"] = Rc5Json::array({fg1Image("probe_image"), fg1Fill("probe_fill", "probe_image")});
        // No explicit pass_set: the first base family must stay main_render.
        root["nodes"][1]["parameters"].erase("pass_set");
    }
    if (!features.empty()) root["rendering"]["features"] = features;
    return root;
}

RenderCompilerProgramResult rc5Compile(const Rc5Json &root, bool xr, const Rc5Json *local_feature = nullptr,
    const std::map<std::string, Rc5Json> &documents = {}, bool fake_facts = false, vk::Extent2D extent = {64,64}) {
    ProgramInputFixture fixture;
    fixture.task_project = std::filesystem::temp_directory_path() /
        ("pelican-rc5-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(fixture.task_project / "features");
    fixture.path_resolver.setup(fixture.task_project, false);
    auto feature = Rc5Json::parse(fixture.path_resolver.loadText("engine://features/ui.json"));
    feature["name"] = "local_ui";
    if (local_feature) feature = *local_feature;
    std::ofstream{fixture.task_project / "features/local.json"} << feature.dump(2);
    std::ofstream{fixture.task_project / "lighting.json"} << fixture.path_resolver.loadText("engine://render_fragments/deferred_lighting.json");
    for (const auto &[name, document] : documents) {
        std::filesystem::create_directories((fixture.task_project / name).parent_path());
        std::ofstream{fixture.task_project / name} << document.dump(2);
    }
    fixture.config = root;
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.backend_context.output_extent=extent;
    if(fake_facts) { fixture.backend_context.planning_facts=TestSupport::rc7cDeviceFacts(); fixture.backend_context.planning_facts->planning_extent=extent; }
    fixture.variants = {{.graph_variant = xr ? RenderPipelineGraphVariant::xr : RenderPipelineGraphVariant::flat}};
    auto input = fixture.input();
    input.runtime_shader_compiler_enabled = true;
    return runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
}

RenderCompilerProgramResult rc5CompileWithFacts(const Rc5Json &root,bool xr,const Rc5Json *feature=nullptr,
    const std::map<std::string,Rc5Json> &documents={}) {
    return rc5Compile(root,xr,feature,documents,true);
}

Rc5Json rc5Snapshot(const RenderCompilerProgramResult &result) {
    const auto &variant = result.variants.at(0);
    REQUIRE(variant.render_graph != nullptr);
    REQUIRE(variant.physical_package != nullptr);
    Rc5Json snapshot{{"typed_ir_fingerprint", renderGraphFingerprint(*variant.render_graph)}};
    const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package);
    for (const auto &[name, frame] : variant.frame_plans) {
        snapshot["frame_plans"][name] = framePlanToJson(frame, variant.compiled_pipeline.get());
        const auto &plan = *physical.target_plans.at(name);
        snapshot["physical_plans"][name] = vulkanTargetPlanToJson(plan);
    }
    return snapshot;
}
} // namespace

TEST_CASE("RC-5s sprite attachments participate in IR and physical planning", "[rendercompiler][rc5s][sprite]") {
    auto root = rc5Document(false, false, false);
    root["rendering"]["features"] = Rc5Json::array({"engine://features/sprite.json"});
    const auto result = rc5Compile(root, false);
    const auto &variant = result.variants.at(0);
    const auto &graph = variant.render_graph->graphs.at(0);
    const auto sprite = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const auto &n) { return n.name == "sprite_draw"; });
    REQUIRE(sprite != graph.nodes.end());
    CHECK(sprite->shape == RenderNodeShape::sprite);
    REQUIRE(sprite->uses.size() == 2);
    for (const auto &use : sprite->uses) {
        CHECK(use.access == ResourceUseAccess::read_write);
        REQUIRE(use.operations.has_value());
        CHECK(use.operations->load_op == AttachmentLoadOp::load);
        CHECK(use.operations->store_op == AttachmentStoreOp::store);
    }
    CHECK(sprite->uses.at(0).resource == "lit_color");
    CHECK(sprite->uses.at(1).resource == "scene_depth");
    CHECK(sprite->uses.at(0).role == ResourceUseRole::color_attachment);
    CHECK(sprite->uses.at(1).role == ResourceUseRole::depth_attachment);
    const auto snapshot = rc5Snapshot(result);
    const auto &physical = snapshot.at("physical_plans").at("main_render");
    const auto scope = std::find_if(physical.at("scopes").begin(), physical.at("scopes").end(), [](const auto &s) {
        return s.at("nodes") == Rc5Json::array({"sprite_draw"});
    });
    REQUIRE(scope != physical.at("scopes").end());
    CHECK(scope->at("kind") == "rendering");
    const auto &frame = variant.frame_plans.at("main_render");
    const auto planned = std::find_if(frame.nodes.begin(), frame.nodes.end(), [](const auto &n) { return n.name == "sprite_draw"; });
    REQUIRE(planned != frame.nodes.end());
    CHECK(planned->reads.size() == 2);
    CHECK(planned->writes.size() == 2);
    const auto has_barrier = [&](std::string_view from, std::string_view to, std::string_view resource) {
        return std::any_of(frame.barriers.begin(), frame.barriers.end(), [&](const auto &barrier) {
            return barrier.from == from && barrier.to == to && barrier.resource == resource;
        });
    };
    CHECK(has_barrier("forward_transparent", "sprite_draw", "lit_color"));
    CHECK(has_barrier("forward_transparent", "sprite_draw", "scene_depth"));
    CHECK(has_barrier("sprite_draw", "scene_present", "lit_color"));
    const auto order = framePlanOrder(frame);
    const auto sprite_order = std::find(order.begin(), order.end(), "sprite_draw") - order.begin();
    for (const auto *name : {"lit_color", "scene_depth"}) {
        const auto resource = std::find_if(physical.at("resources").begin(), physical.at("resources").end(),
            [&](const auto &entry) { return entry.at("logical_resource") == name; });
        REQUIRE(resource != physical.at("resources").end());
        CHECK(resource->at("lifetime").at("used") == true);
        CHECK(resource->at("lifetime").at("first_use").get<std::size_t>() <= sprite_order);
        CHECK(resource->at("lifetime").at("last_use").get<std::size_t>() >= sprite_order);
        const auto attachment = std::find_if(physical.at("attachments").begin(), physical.at("attachments").end(),
            [&](const auto &entry) { return entry.at("node") == "sprite_draw" && entry.at("logical_resource") == name; });
        REQUIRE(attachment != physical.at("attachments").end());
        CHECK(attachment->at("load_op") == "load");
        CHECK(attachment->at("store_op") == "store");
    }
}

TEST_CASE("RC-5 fixed HEAD compiler baseline", "[rendercompiler][rc5-baseline]") {
    for (const bool lit : {false, true}) for (const bool hdr : {false, true})
    for (const bool extras : {false, true}) for (const bool xr : {false, true}) {
#if !PELICAN_WITH_OPENXR
        if (xr) continue;
#endif
        CAPTURE(lit, hdr, extras, xr);
        const auto snapshot = rc5Snapshot(rc5Compile(rc5Document(lit, hdr, extras), xr));
        if (const auto *directory = std::getenv("RC5_RECORD_DIR")) {
            const auto filename = std::string{lit ? "lit" : "bare"} + (hdr ? "_hdr" : "_ldr") +
                (extras ? "_extras" : "_empty") + (xr ? "_xr.json" : "_flat.json");
            std::filesystem::create_directories(directory);
            std::ofstream{std::filesystem::path{directory} / filename} << snapshot.dump(2);
        }
    }
}

namespace {
std::string rc5Filename(bool lit, bool hdr, bool extras, bool xr) {
    return std::string{lit ? "lit" : "bare"} + (hdr ? "_hdr" : "_ldr") +
        (extras ? "_extras" : "_empty") + (xr ? "_xr.json" : "_flat.json");
}

Rc5Json rc5Comparable(Rc5Json snapshot) {
    for (auto &[name, frame] : snapshot.at("frame_plans").items()) {
        frame.erase("pipeline_preset");
        auto &diagnostics = frame["diagnostics"];
        diagnostics.erase(std::remove_if(diagnostics.begin(), diagnostics.end(), [](const auto &d) {
            return d.at("kind") == "pipeline_preset_resolved";
        }), diagnostics.end());
        for (auto &node : frame.at("nodes")) node.erase("fragment");
        for (auto &resource : frame.at("resources")) resource.erase("fragment");
    }
    return snapshot;
}

void rc5RequireFragmentProvenance(const RenderCompilerProgramResult &result, bool replacement) {
    const auto &pipeline = *result.variants.at(0).compiled_pipeline;
    const std::vector<std::string> names{"deferred_geometry", "ssao", "deferred_lighting", "forward_base", "present"};
    std::map<std::string, std::string> passes, targets;
    for (const auto &name : names) {
        const auto document = Rc5Json::parse(PathResolver{}.loadText("engine://render_fragments/" + name + ".json"));
        for (const auto &p : document.at("passes")) passes[p.at("name").get<std::string>()] = name;
        for (const auto &t : document.at("render_targets")) targets[t.at("name").get<std::string>()] = name;
    }
    std::size_t pass_count = 0, target_count = 0;
    const auto check = [&](const auto &entry, const auto &expected, const std::string &authored) {
        if (const auto found = expected.find(authored); found != expected.end()) {
            REQUIRE(entry.fragment.has_value());
            CHECK(entry.fragment->name == found->second);
            CHECK(entry.fragment->reference == (replacement && found->second == "deferred_lighting"
                ? "project://lighting.json" : "engine://render_fragments/" + found->second + ".json"));
            CHECK(entry.source == RenderPipelineProvenanceSource::engine);
        } else REQUIRE_FALSE(entry.fragment.has_value());
    };
    for (const auto &pass : pipeline.pass_provenance) {
        check(pass, passes, pass.authored_name);
        if (pass.fragment) ++pass_count;
    }
    for (const auto &target : pipeline.resource_provenance) {
        check(target, targets, target.name);
        if (target.fragment) ++target_count;
    }
    CHECK(pass_count == 9);
    CHECK(target_count == 11);
    const auto snapshot = rc5Snapshot(result);
    for (const auto &[name, frame] : snapshot.at("frame_plans").items()) {
        for (const auto &node : frame.at("nodes"))
            if (passes.contains(node.value("authored_name", ""))) REQUIRE(node.contains("fragment"));
        for (const auto &target : frame.at("resources"))
            if (targets.contains(target.at("name").get<std::string>())) REQUIRE(target.contains("fragment"));
    }
}
}

void rc5RequireRecipeMatrix() {
    std::size_t configurations = 0;
    for (const bool lit : {false, true}) for (const bool hdr : {false, true})
    for (const bool extras : {false, true}) for (const bool xr : {false, true}) {
#if !PELICAN_WITH_OPENXR
        if (xr) continue;
#endif
        CAPTURE(lit, hdr, extras, xr);
        const auto old_root = rc5Document(lit, hdr, extras);
        const auto old_result = rc5Compile(old_root, xr);
        const auto original = rc5Snapshot(old_result);
        for (const auto &graph : old_result.variants.at(0).render_graph->graphs)
            for (const auto &node : graph.nodes)
                CHECK(node.name.rfind("__anchor_", 0) != 0);
        for (const auto &[name, frame] : original.at("frame_plans").items()) {
            for (const auto &node : frame.at("nodes"))
                CHECK(node.at("name").get<std::string>().rfind("__anchor_", 0) != 0);
        }
        for (const auto &[name, physical] : original.at("physical_plans").items())
            for (const auto &scope : physical.at("scopes"))
                for (const auto &node : scope.at("nodes"))
                    CHECK(node.get<std::string>().rfind("__anchor_", 0) != 0);
        ++configurations;
        const auto &physical_packages = requireVulkanRenderCompilerPhysicalPackage(*old_result.variants.at(0).physical_package);
        for (const auto &[name, plan] : physical_packages.target_plans) {
            const auto &context = requireVulkanTargetPlanVerificationContext(physical_packages, name);
            const auto verify = [&](VulkanCompletePhysicalPlanPackage package) {
                return verifyVulkanCompletePhysicalPlanPackage(*context.logical_graph, context.topology,
                    *context.automatic_plan, context.node_order, std::move(package), context.format_capabilities,
                    context.enabled_device_extensions);
            };
            const auto fresh = ejectVulkanCompletePhysicalPlanPackage(*plan);
            const auto reapplied = applyVerifiedVulkanCompletePhysicalPlanPackage(*plan, verify(fresh));
            CHECK(physicalNodeOrderOf(reapplied.scopes).nodes == framePlanOrder(old_result.variants.at(0).frame_plans.at(name)));
            CHECK(vulkanCompletePhysicalPlanPackageToJson(ejectVulkanCompletePhysicalPlanPackage(reapplied)) ==
                  vulkanCompletePhysicalPlanPackageToJson(fresh));
        }
        auto root = rc5Document(false, hdr, extras);
        root["rendering"]["pipeline"]["preset"] = lit
            ? "engine://render_pipelines/experimental/hybrid_recipe_lit.json"
            : "engine://render_pipelines/experimental/hybrid_recipe.json";
        const auto result = rc5Compile(root, xr);
        // This compares every typed field, including anchors, snapshots, attachments,
        // image/buffer declaration order, uses, dependencies and views.
        REQUIRE(*result.variants.at(0).render_graph == *old_result.variants.at(0).render_graph);
        const auto current = rc5Snapshot(result);
        if (const auto *directory = std::getenv("RC5_CURRENT_DIR")) {
            std::filesystem::create_directories(directory);
            std::ofstream{std::filesystem::path{directory} / rc5Filename(lit,hdr,extras,xr)} << current.dump(2);
        }
        REQUIRE(rc5Comparable(current) == rc5Comparable(original));
        rc5RequireFragmentProvenance(result, false);
        auto replaced_root = root;
        replaced_root["rendering"]["fragment_overrides"] = {{"deferred_lighting", "project://lighting.json"}};
        const auto replaced = rc5Compile(replaced_root, xr);
        REQUIRE(*replaced.variants.at(0).render_graph == *result.variants.at(0).render_graph);
        REQUIRE(rc5Comparable(rc5Snapshot(replaced)) == rc5Comparable(current));
        rc5RequireFragmentProvenance(replaced, true);
    }
#if PELICAN_WITH_OPENXR
    CHECK(configurations == 16);
#else
    CHECK(configurations == 8);
#endif
}

TEST_CASE("RC-5 A2 recipes preserve typed IR frame plans physical plans and complete packages", "[rendercompiler][rc5][rc5-a2]") {
    rc5RequireRecipeMatrix();
}

TEST_CASE("RC-5s fixed HEAD complete packages are stale", "[rendercompiler][rc5s][fixed-head-package]") {
    const auto *directory = std::getenv("RC5_BASELINE_DIR");
    if (!directory || !*directory) SKIP("rc5s.fixed_head_packages_unavailable@1: set RC5_BASELINE_DIR");
    std::size_t configurations = 0;
    for (const bool lit : {false, true}) for (const bool hdr : {false, true})
    for (const bool extras : {false, true}) for (const bool xr : {false, true}) {
#if !PELICAN_WITH_OPENXR
        if (xr) continue;
#endif
        CAPTURE(lit, hdr, extras, xr);
        const auto frozen = readJsonFixture(std::filesystem::path{directory} / rc5Filename(lit, hdr, extras, xr));
        const auto result = rc5Compile(rc5Document(lit, hdr, extras), xr);
        const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*result.variants.at(0).physical_package);
        for (const auto &[name, plan] : physical.target_plans) {
            const auto &context = requireVulkanTargetPlanVerificationContext(physical, name);
            const auto stale = vulkanCompletePhysicalPlanPackageFromJson(
                frozen.at("physical_plans").at(name).at("ejectable_complete_physical_plan"));
            REQUIRE_THROWS_WITH(verifyVulkanCompletePhysicalPlanPackage(*context.logical_graph, context.topology,
                *context.automatic_plan, context.node_order, stale, context.format_capabilities,
                context.enabled_device_extensions), Catch::Matchers::ContainsSubstring("stale"));
        }
        ++configurations;
    }
#if PELICAN_WITH_OPENXR
    CHECK(configurations == 16);
#else
    CHECK(configurations == 8);
#endif
}

TEST_CASE("RC5b materialized fragment preserves typed IR and every plan field", "[rendercompiler][rc5b]") {
    for (const bool lit : {false, true}) for (const bool hdr : {false, true})
    for (const bool extras : {false, true}) for (const bool xr : {false, true}) {
#if !PELICAN_WITH_OPENXR
        if (xr) continue;
#endif
        CAPTURE(lit, hdr, extras, xr);
        auto root = rc5Document(false, hdr, extras);
        root["rendering"]["pipeline"]["preset"] = lit
            ? "engine://render_pipelines/experimental/hybrid_recipe_lit.json"
            : "engine://render_pipelines/experimental/hybrid_recipe.json";
        const auto before = rc5Compile(root, xr);
        root["rendering"]["fragment_overrides"] = {{"deferred_lighting", "project://lighting.json"}};
        const auto after = rc5Compile(root, xr);
        REQUIRE(*before.variants.at(0).render_graph == *after.variants.at(0).render_graph);
        auto expected = rc5Snapshot(before);
        const auto replace_origin = [&](auto &&self, Rc5Json &value) -> void {
            if (value.is_object()) {
                if (value.contains("fragment") && value.at("fragment").value("name", std::string{}) == "deferred_lighting")
                    value["fragment"]["reference"] = "project://lighting.json";
                for (auto &child : value) self(self, child);
            } else if (value.is_array()) for (auto &child : value) self(self, child);
        };
        replace_origin(replace_origin, expected);
        // Every remaining field of FramePlan and the physical plan;
        // the copied document's ownership reference is the only allowed change.
        REQUIRE(rc5Snapshot(after) == expected);
        rc5RequireFragmentProvenance(after, true);
    }
}

// RC-6b2: only these retired fields may differ from the immutable RC-5 snapshot.
Rc5Json rc6WithoutRemovedPackages(Rc5Json snapshot) {
    snapshot.erase("pins");
    snapshot.erase("physical_fragments");
    for (auto &[name, plan] : snapshot.at("physical_plans").items())
        for (const auto *field : {"ejectable_pin_package", "ejectable_physical_fragment",
                                 "applied_pin_package", "applied_physical_fragment",
                                 "ejectable_complete_physical_plan"})
            plan.erase(field);
    return snapshot;
}

// RC-6c: explicit resource -> final graph-use node table, independent of
// the lifetime algorithm. Optional feature resources are checked when present;
// the unchanged resource set is still compared with the fixed baseline.
void rc6RequireLifetimes(const RenderCompilerProgramResult &result, bool xr) {
    const std::map<std::string, std::string> last_node{
        {"display", "output_transform"}, {"swapchain", "output_transform"},
        {"g_emissive", "deferred_lighting"}, {"gbuffer_albedo", "deferred_lighting"},
        {"gbuffer_material", "deferred_lighting"}, {"gbuffer_normal", "deferred_lighting"},
        {"gbuffer_worldpos", "deferred_lighting"}, {"ssao_blur", "deferred_lighting"},
        {"ssao_output", "ssao_blur_pass"}, {"lit_color", "scene_present"},
        {"opaque_color", "forward_transparent"}, {"opaque_depth", "forward_transparent"},
        {"scene_depth", "forward_transparent"}, {"shadow_map", "forward_transparent"},
        {"scene_ldr_in", "hdr_tonemap"}, {"probe_image", "probe_fill"}};
    const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*result.variants.at(0).physical_package);
    for (const auto &[name, plan] : physical.target_plans) {
        const auto &context = requireVulkanTargetPlanVerificationContext(physical, name);
        REQUIRE(plan->automatic_plan_fingerprint == vulkanAutomaticTargetPlanFingerprint(context.topology, *plan));
        const auto &nodes = context.node_order.nodes();
        const auto check = [&](const std::string &resource, const TargetResourceLifetime &lifetime) {
            CAPTURE(resource, name);
            REQUIRE(last_node.contains(resource));
            const auto node = std::find(nodes.begin(), nodes.end(), last_node.at(resource));
            REQUIRE(node != nodes.end());
            REQUIRE(lifetime.used);
            CHECK(lifetime.last_use == static_cast<std::size_t>(node - nodes.begin()));
            const auto expected = resource == "swapchain" ? std::vector<std::string>{"present"} :
                resource == "display" && xr ? std::vector<std::string>{"xr_mirror"} : std::vector<std::string>{};
            CHECK(lifetime.retain == expected);
        };
        for (const auto &r : plan->resources) check(r.logical_resource, r.lifetime);
        for (const auto &r : plan->lowering_graph.resources) check(r.logical.name, r.lifetime);
        for (const auto &r : plan->lowering_graph.resources)
            if (r.logical.materialization == LogicalMaterializationRequirement::required)
                CHECK_FALSE((r.pattern.allow_alias && !r.pattern.require_store));
    }
}

// Compare the inputs of actual GPU allocation with the frozen physical fields,
// through generation-resource resolution and the registration CPU seam.
void rc6RequireRegisteredAllocations(const RenderCompilerProgramResult &result, const Rc5Json &recorded) {
    const auto &variant = result.variants.at(0);
    const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package);
    REQUIRE(physical.target_plans.size() == 1);
    const auto &frozen = recorded.at("physical_plans").begin().value();
    auto legacy = frozen.at("ejectable_complete_physical_plan");
    legacy["memory_regions"]=Rc5Json::array();
    legacy["memory_packing"]=memoryPackingToJson({});
    const auto package = vulkanCompletePhysicalPlanPackageFromJson(legacy);
    std::map<std::string, const VulkanPhysicalResourcePlan *> by_name;
    std::map<std::string, std::string> alias;
    for (const auto &r : package.resources) by_name.emplace(r.logical_resource, &r);
    for (const auto &g : package.alias_groups) {
        // Registration names a merged runtime allocation by its members,
        // independently of the graph-local physical group ID.
        std::string runtime_id = "runtime-alias";
        for (const auto &name : g.resources) runtime_id += "/" + name;
        for (const auto &name : g.resources) alias.emplace(name, runtime_id);
    }
    auto expected_placements = physical.image_placements;
    for (auto &p : expected_placements) {
        const auto r = by_name.find(p.resource);
        if (r == by_name.end()) continue; // Unused declarations remain allocated.
        p.format = *r->second->format;
        p.samples = r->second->rasterization_samples;
        p.array_layers = r->second->array_layers;
        p.storage = r->second->representation == VulkanResourceRepresentation::tile_local_attachment
            ? RenderTargetStorageMode::tile_local_attachment :
            r->second->representation == VulkanResourceRepresentation::transient_attachment
                ? RenderTargetStorageMode::transient_attachment : RenderTargetStorageMode::materialized;
        p.alias_group.reset(); // compiler_only has no requirements, so no sharing.
        p.local_read = r->second->representation == VulkanResourceRepresentation::tile_local_attachment;
        p.external_depth_source = package.external_depth_export && package.external_depth_export->source_resource == p.resource;
    }
    CHECK(physical.image_placements == expected_placements);
    const GenerationResourceVariant frozen_variant{.name = "reference", .render_graph = variant.render_graph.get(),
        .placements = expected_placements, .runtime_layered_images = runtimeLayeredImagesOf(*variant.compiled_pipeline)};
    const auto expected = resolveGenerationResources(std::span{&frozen_variant, 1}, {64, 64});
    REQUIRE(result.generation_resources.images == expected.images);
    std::vector<RenderTargetDefinition> actual_targets, expected_targets;
    for (const auto &image : result.generation_resources.images) actual_targets.push_back(renderTargetDefinitionOf(image));
    for (const auto &image : expected.images) expected_targets.push_back(renderTargetDefinitionOf(image));
    const auto actual = test_support::registerRenderTargetsWithoutDevice(actual_targets, {64, 64});
    const auto baseline = test_support::registerRenderTargetsWithoutDevice(expected_targets, {64, 64});
    REQUIRE(actual.size() == baseline.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        CHECK(actual[i].metadata.name == baseline[i].metadata.name);
        CHECK(actual[i].alias_group == baseline[i].alias_group);
        CHECK(renderTargetBytes(actual[i].metadata) == renderTargetBytes(baseline[i].metadata));
    }
}

Rc5Json rc6WithoutLifetimeChanges(Rc5Json snapshot) {
    snapshot = rc6WithoutRemovedPackages(std::move(snapshot));
    // Only these exact paths: physical_plans.*.automatic_plan_fingerprint,
    // physical_plans.*.{resources,lowering_graph.resources}[*].lifetime.{last_use,retain}.
    // used, first_use, cost, aliases, representations and attachment ops remain.
    for (auto &[name, plan] : snapshot.at("physical_plans").items()) {
        plan.erase("automatic_plan_fingerprint");
        for (auto *resources : {&plan.at("resources"), &plan.at("lowering_graph").at("resources")})
            for (auto &r : *resources) {
                r.at("lifetime").erase("last_use");
                r.at("lifetime").erase("retain");
            }
    }
    return snapshot;
}

// RC-7b: remove only allocation-derived fields. Explanatory text is checked
// against the precise change in the former fixed-usage rule before restoring
// the baseline spelling; representation/ops/usage/lifetimes stay comparable.
std::pair<Rc5Json, Rc5Json> rc7bComparable(Rc5Json actual, Rc5Json recorded) {
    for (auto &[name, plan] : actual.at("physical_plans").items()) {
        auto &baseline = recorded.at("physical_plans").at(name);
        for (std::size_t i = 0; i < plan.at("resources").size(); ++i) {
            auto &r = plan.at("resources")[i];
            const auto &old = baseline.at("resources")[i];
            REQUIRE(r.at("logical_resource") == old.at("logical_resource"));
            if (r.at("reason") != old.at("reason")) {
                const auto former = old.at("reason").get<std::string>();
                REQUIRE(former.find("alias-safe set") != std::string::npos);
                const auto expected = r.at("logical_resource") == "display"
                    ? "logical materialization requirement is required: display-format target is not aliased"
                    : "arbitrary read requires a materialized resource";
                REQUIRE(r.at("reason") == expected);
                for (auto &d : plan.at("decisions"))
                    if (d.at("id") == "pelican.plan.resource_representation@1" && d.at("subject") == r.at("logical_resource")) {
                        REQUIRE(d.at("detail") == expected);
                        d["detail"] = old.at("reason");
                    }
                r["reason"] = old.at("reason");
            }
        }
        auto &decisions = plan.at("decisions");
        decisions.erase(std::remove_if(decisions.begin(), decisions.end(), [](const auto &d) {
            return d.at("id") == "pelican.plan.alias_ineligible@1" || d.at("id") == "pelican.plan.alias_pair_ineligible@1";
        }), decisions.end());
        for (auto *p : {&plan, &baseline}) {
            p->erase("alias_groups");
            p->erase("memory_regions");
            p->erase("memory_packing");
            p->erase("memory_types");p->erase("image_memory_facts");
            auto &placement_decisions=p->at("decisions");
            placement_decisions.erase(std::remove_if(placement_decisions.begin(),placement_decisions.end(),[](const auto &d){const auto id=d.at("id").template get<std::string>(); return id=="pelican.plan.alias_requirements_unknown@1" || id=="pelican.plan.alias_group_selected@1";}),placement_decisions.end());
            p->erase("automatic_plan_fingerprint");
            p->erase("logical_graph_fingerprint");
            (*p).at("planning_opportunities").erase("alias_candidates");
            auto &opportunity_decisions=p->at("planning_opportunities").at("decisions");
            opportunity_decisions.erase(std::remove_if(opportunity_decisions.begin(),opportunity_decisions.end(),[](const auto &d){return d.at("id")=="pelican.plan.alias_stress_candidates@1";}),opportunity_decisions.end());
            for (auto &r : p->at("resources")) r.erase("aliasable");
            for (auto &r : p->at("lowering_graph").at("resources")) {
                r.erase("logical_materialization");
                r.at("pattern").erase("allow_alias");
                r.at("pattern").erase("require_store");
            }
        }
    }
    return {std::move(actual), std::move(recorded)};
}

TEST_CASE("RC-6 policy-free snapshots match all sixteen fixed RC-5 baselines", "[.][rc6-baseline-compare]") {
    const auto *directory = std::getenv("RC6_BASELINE_DIR");
    REQUIRE(directory != nullptr);
    REQUIRE(std::string{directory}.size() > 0);
#if !PELICAN_WITH_OPENXR
    FAIL("RC-6 baseline comparison requires XR and all sixteen configurations");
#endif
    std::size_t count = 0;
    for (const bool lit : {false, true}) for (const bool hdr : {false, true})
    for (const bool extras : {false, true}) for (const bool xr : {false, true}) {
        const auto file = std::filesystem::path{directory} / rc5Filename(lit, hdr, extras, xr);
        CAPTURE(file);
        std::ifstream input{file};
        REQUIRE(input.good());
        const auto recorded = Rc5Json::parse(input);
        const auto result = rc5Compile(rc5Document(lit, hdr, extras), xr);
        rc6RequireLifetimes(result, xr);
        rc6RequireRegisteredAllocations(result, recorded);
        const auto comparable = rc7bComparable(rc6WithoutLifetimeChanges(rc5Snapshot(result)), rc6WithoutLifetimeChanges(recorded));
        REQUIRE(comparable.first == comparable.second);
        ++count;
    }
    REQUIRE(count == 16);
}

TEST_CASE("RC-6b2 saved legacy planning keys are refused with reload instructions", "[rendercompiler][rc6b2][legacy-package-removed]") {
    for (const auto *key : {"vulkan_plan_pins", "vulkan_physical_fragments"})
    for (const auto &value : {Rc5Json{}, Rc5Json::object(), Rc5Json{{"flat", Rc5Json::array()}}})
    for (const bool policy : {false, true}) {
        CAPTURE(key, value, policy);
        auto root = rc5Document(false, false, false);
        root["rendering"][key] = value;
        if (policy) root["rendering"]["target_planning"] = {{"graphs", {{"main_render", {{"backend", "materialized"}}}}}};
        try { (void)rc5Compile(root, false); FAIL("removed key was accepted"); }
        catch (const GraphRegistryError &error) {
            CHECK(error.code() == "target_planning.legacy_package_removed@1");
            CHECK(error.detail().at("key") == key);
            const auto message = error.detail().at("message").get<std::string>();
            CHECK(message.find(key) != std::string::npos);
            CHECK(message.find("from the document and reload") != std::string::npos);
            CHECK(message.find("Studio Pin / Freeze") != std::string::npos);
        }
    }
}

TEST_CASE("RC-6b2 complete stale refusal and install identity survive legacy removal", "[rendercompiler][rc6b2][complete-stale]") {
    const auto result = rc5Compile(rc5Document(false, false, false), false);
    const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*result.variants.at(0).physical_package);
    for (const auto &[name, plan] : physical.target_plans) {
        const auto &context = requireVulkanTargetPlanVerificationContext(physical, name);
        REQUIRE(context.automatic_plan == plan);
        auto package = ejectVulkanCompletePhysicalPlanPackage(*plan);
        SECTION("stale logical graph") {
            package.logical_graph_fingerprint ^= 1;
            CHECK_THROWS_WITH(verifyVulkanCompletePhysicalPlanPackage(*context.logical_graph, context.topology,
                *context.automatic_plan, context.node_order, package, context.format_capabilities,
                context.enabled_device_extensions), Catch::Matchers::ContainsSubstring("stale for the logical graph"));
        }
        SECTION("stale automatic plan") {
            package.automatic_plan_fingerprint ^= 1;
            CHECK_THROWS_WITH(verifyVulkanCompletePhysicalPlanPackage(*context.logical_graph, context.topology,
                *context.automatic_plan, context.node_order, package, context.format_capabilities,
                context.enabled_device_extensions), Catch::Matchers::ContainsSubstring("complete_physical_plan.stale_automatic_plan@1"));
        }
        SECTION("installed plan must be the retained automatic object") {
            auto changed = physical;
            changed.target_plans[name] = std::make_shared<const VulkanTargetPlan>(*plan);
            CHECK_THROWS_WITH(installVerifiedVulkanCompletePhysicalPlanPackage(changed, name, package),
                Catch::Matchers::ContainsSubstring("modified before complete plan verification"));
        }
        SECTION("fresh complete package installs") {
            auto fresh = physical;
            REQUIRE_NOTHROW(installVerifiedVulkanCompletePhysicalPlanPackage(fresh, name, package));
            CHECK(fresh.verified_complete_physical_plans.contains(name));
            CHECK(fresh.target_plans.at(name) != context.automatic_plan);
        }
    }
}

TEST_CASE("RC-7a F4 unchanged HEAD baseline including fingerprints", "[.][rendercompiler][rc7a][rc7-baseline-compare]") {
    const std::filesystem::path directory{"<作業ディレクトリ>/rc7_baseline/rc5"};
    REQUIRE(std::filesystem::is_directory(directory));
    std::size_t count = 0;
    for (const bool lit : {false, true}) for (const bool hdr : {false, true})
    for (const bool extras : {false, true}) for (const bool xr : {false, true}) {
        const auto filename = directory / rc5Filename(lit, hdr, extras, xr);
        CAPTURE(filename.string());
        REQUIRE(std::filesystem::is_regular_file(filename));
        const auto expected = Rc5Json::parse(std::ifstream{filename});
        const auto actual = rc5Snapshot(rc5Compile(rc5Document(lit, hdr, extras), xr));
        const auto comparable = rc7bComparable(actual, expected);
        CHECK(comparable.first == comparable.second);
        ++count;
    }
    CHECK(count == 16);
}

TEST_CASE("RC-7b B4 semantic fields match sixteen immutable preimplementation snapshots", "[.][rc7b][rc7b-baseline-compare]") {
    const std::filesystem::path directory{"<作業ディレクトリ>/rc7b_baseline/rc5"};
    REQUIRE(std::filesystem::is_directory(directory));
    std::size_t count = 0;
    Rc5Json differences = Rc5Json::object();
    for (const bool lit : {false, true}) for (const bool hdr : {false, true})
    for (const bool extras : {false, true}) for (const bool xr : {false, true}) {
        const auto filename = rc5Filename(lit, hdr, extras, xr);
        CAPTURE(filename);
        const auto expected = Rc5Json::parse(std::ifstream{directory / filename});
        const auto actual = rc5Snapshot(rc5Compile(rc5Document(lit, hdr, extras), xr));
        differences[filename] = Rc5Json::diff(expected, actual);
        const auto comparable = rc7bComparable(actual, expected);
        CHECK(comparable.first == comparable.second);
        ++count;
    }
    CHECK(count == 16);
    if (const auto *output = std::getenv("RC7B_DIFF_PATH")) std::ofstream{output} << differences.dump(2);
}

TEST_CASE("RC-7c C5 sixteen fixed snapshots retain every semantic field", "[.][rc7c-baseline-compare]") {
    const std::filesystem::path directory{"<作業ディレクトリ>/rc7c_baseline/rc5"};
    REQUIRE(std::filesystem::is_directory(directory));
    Rc5Json report=Rc5Json::object();
    std::size_t count=0;
    const auto comparable=[](Rc5Json snapshot) {
        snapshot=rc6WithoutRemovedPackages(std::move(snapshot));
        for(auto &[name,plan]:snapshot.at("physical_plans").items()) {
            for(const auto *field:{"memory_regions","memory_packing","alias_groups","automatic_plan_fingerprint","memory_types","image_memory_facts"}) plan.erase(field);
            plan.at("planning_opportunities").erase("alias_candidates");
            auto &opportunity_decisions=plan.at("planning_opportunities").at("decisions");
            opportunity_decisions.erase(std::remove_if(opportunity_decisions.begin(),opportunity_decisions.end(),[](const auto &d){return d.at("id")=="pelican.plan.alias_stress_candidates@1";}),opportunity_decisions.end());
            for(auto &r:plan.at("resources"))r.erase("aliasable");
            for(auto &r:plan.at("lowering_graph").at("resources"))r.at("pattern").erase("allow_alias");
            auto &decisions=plan.at("decisions");
            decisions.erase(std::remove_if(decisions.begin(),decisions.end(),[](const auto &d){const auto id=d.at("id").template get<std::string>();
                return id=="pelican.plan.alias_group_selected@1" || id=="pelican.plan.alias_requirements_unknown@1" ||
                    id=="pelican.plan.alias_pair_ineligible@1" || id=="pelican.plan.alias_ineligible@1";}),decisions.end());
        }
        return snapshot;
    };
    for(const bool lit:{false,true})for(const bool hdr:{false,true})for(const bool extras:{false,true})for(const bool xr:{false,true}) {
        const auto filename=rc5Filename(lit,hdr,extras,xr);CAPTURE(filename);
        const auto old=Rc5Json::parse(std::ifstream{directory/filename});
        const auto now=rc5Snapshot(rc5Compile(rc5Document(lit,hdr,extras),xr));
        CHECK(comparable(now)==comparable(old));
        report[filename]["raw_diff"]=Rc5Json::diff(old,now);
        for(const auto &[graph,plan]:now.at("physical_plans").items()) {
            CHECK(plan.at("alias_groups").empty());CHECK(plan.at("memory_regions").empty());
            for(const auto &r:plan.at("resources")) {
                const auto resource=r.at("logical_resource").get<std::string>();
                bool formerly_shared=false;
                for(const auto &group:old.at("physical_plans").at(graph).at("alias_groups"))
                    for(const auto &member:group.at("resources"))formerly_shared=formerly_shared || member==resource;
                report[filename][graph][resource]={{"previously_shared",formerly_shared},
                    {"reason",formerly_shared?"requirements unknown: previous sharing removed":"requirements unknown: remains separate"}};
            }
        }
        ++count;
    }
    CHECK(count==16);
    // The bundled example, planned with a deterministic CPU device at 1920x1080.
    const auto example=Rc5Json::parse(std::ifstream{std::filesystem::path{PELICAN_TEST_SOURCE_DIR}/"projects/example/passes/main_rendering_config.json"});
    const auto result=rc5Compile(example,false,nullptr,{},true,{1920,1080});
    const auto &physical=requireVulkanRenderCompilerPhysicalPackage(*result.variants[0].physical_package);
    for(const auto &[graph,plan]:physical.target_plans) {
        report["example_1920x1080"][graph]["memory_regions"]=memoryRegionsToJson(plan->memory_regions);
        for(const auto &region:plan->memory_regions)for(const auto &r:region.residents) {
            CHECK(r.offset%r.alignment==0);CHECK(r.offset+r.size<=region.size);
        }
    }
    std::ofstream{"<作業ディレクトリ>/rc7d_c5_details.json"}<<report.dump(2);
}


TEST_CASE("GF-1d preset graph filters do not affect the compiled plan", "[rendercompiler][rc5][gf1d]") {
    const auto recipe = Rc5Json::parse(PathResolver{}.loadText(
        "engine://render_pipelines/experimental/hybrid_recipe.json"));
    auto without_filters = recipe;
    without_filters["config"].erase("graph_filters");
    const std::map<std::string, Rc5Json> documents{
        {"render_pipelines/with_filters.json", recipe},
        {"render_pipelines/without_filters.json", without_filters},
    };
    auto with_root = rc5Document(false, false, false);
    auto without_root = with_root;
    with_root["rendering"]["pipeline"]["preset"] = "project://render_pipelines/with_filters.json";
    without_root["rendering"]["pipeline"]["preset"] = "project://render_pipelines/without_filters.json";
    CHECK(rc5Comparable(rc5Snapshot(rc5Compile(with_root, false, nullptr, documents))) ==
          rc5Comparable(rc5Snapshot(rc5Compile(without_root, false, nullptr, documents))));
}
