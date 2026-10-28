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
    const std::map<std::string, Rc5Json> &documents = {}) {
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
    fixture.variants = {{.graph_variant = xr ? RenderPipelineGraphVariant::xr : RenderPipelineGraphVariant::flat}};
    auto input = fixture.input();
    input.runtime_shader_compiler_enabled = true;
    return runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
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
        snapshot["pins"][name] = vulkanTargetPlanPinPackageToJson(ejectVulkanTargetPlanPinPackage(plan));
        snapshot["physical_fragments"][name] = vulkanPhysicalFragmentPackageToJson(ejectVulkanPhysicalFragmentPackage(plan));
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

void rc5RequireRecipeMatrix(bool fixed_head_packages) {
    const auto *directory = std::getenv("RC5_BASELINE_DIR");
    if (fixed_head_packages) REQUIRE((directory != nullptr && *directory != '\0'));
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
        Rc5Json frozen;
        if (fixed_head_packages) {
            frozen = readJsonFixture(std::filesystem::path{directory} / rc5Filename(lit, hdr, extras, xr));
            REQUIRE(original.at("typed_ir_fingerprint") != frozen.at("typed_ir_fingerprint"));
        }
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
            if (fixed_head_packages) {
                const auto stale = vulkanCompletePhysicalPlanPackageFromJson(
                    frozen.at("physical_plans").at(name).at("ejectable_complete_physical_plan"));
                REQUIRE_THROWS_WITH(verify(stale), Catch::Matchers::ContainsSubstring("stale"));
            }
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
        const auto variant_name = xr ? "xr" : "flat";
        for (const auto *key : {"vulkan_plan_pins", "vulkan_physical_fragments"}) {
            auto pinned = root;
            auto packages = Rc5Json::array();
            for (const auto &[name, package] : original.at(std::string{key} == "vulkan_plan_pins" ? "pins" : "physical_fragments").items())
                packages.push_back(package);
            pinned["rendering"][key][variant_name] = packages;
            if (fixed_head_packages) {
                auto stale = pinned;
                auto old_packages = Rc5Json::array();
                for (const auto &[name, package] : frozen.at(std::string{key} == "vulkan_plan_pins" ? "pins" : "physical_fragments").items())
                    old_packages.push_back(package);
                stale["rendering"][key][variant_name] = old_packages;
                REQUIRE_THROWS_AS(rc5Compile(stale, xr), std::exception);
            }
            const auto applied = rc5Compile(pinned, xr);
            REQUIRE(*applied.variants.at(0).render_graph == *result.variants.at(0).render_graph);
            const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*applied.variants.at(0).physical_package);
            for (const auto &[name, plan] : applied.variants.at(0).frame_plans) {
                REQUIRE(physicalNodeOrderOf(physical.target_plans.at(name)->scopes).nodes == framePlanOrder(plan));
                if (std::string{key} == "vulkan_plan_pins") {
                    REQUIRE(physical.target_plans.at(name)->applied_pin_package.has_value());
                    REQUIRE(Rc5Json(vulkanTargetPlanPinPackageToJson(*physical.target_plans.at(name)->applied_pin_package)) == original.at("pins").at(name));
                } else {
                    REQUIRE(physical.target_plans.at(name)->applied_fragment_package.has_value());
                    REQUIRE(Rc5Json(vulkanPhysicalFragmentPackageToJson(*physical.target_plans.at(name)->applied_fragment_package)) == original.at("physical_fragments").at(name));
                }
            }
        }
    }
#if PELICAN_WITH_OPENXR
    CHECK(configurations == 16);
#else
    CHECK(configurations == 8);
#endif
}

TEST_CASE("RC-5 A2 recipes preserve typed IR frame plans physical plans and old packages", "[rendercompiler][rc5][rc5-a2]") {
    rc5RequireRecipeMatrix(false);
}

TEST_CASE("RC-5s fixed HEAD complete fragment and pin packages are stale", "[rendercompiler][rc5s][fixed-head-package]") {
    const auto *directory = std::getenv("RC5_BASELINE_DIR");
    if (!directory || !*directory) SKIP("rc5s.fixed_head_packages_unavailable@1: set RC5_BASELINE_DIR");
    rc5RequireRecipeMatrix(true);
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
        // Every field of FramePlan, physical plan, pins and physical fragments;
        // the copied document's ownership reference is the only allowed change.
        REQUIRE(rc5Snapshot(after) == expected);
        rc5RequireFragmentProvenance(after, true);
    }
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
        REQUIRE(rc5Snapshot(rc5Compile(rc5Document(lit, hdr, extras), xr)) == recorded);
        ++count;
    }
    REQUIRE(count == 16);
}
