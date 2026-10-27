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

RenderCompilerProgramResult rc5Compile(const Rc5Json &root, bool xr) {
    ProgramInputFixture fixture;
    fixture.task_project = std::filesystem::temp_directory_path() /
        ("pelican-rc5-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(fixture.task_project / "features");
    fixture.path_resolver.setup(fixture.task_project, false);
    auto feature = Rc5Json::parse(fixture.path_resolver.loadText("engine://features/ui.json"));
    feature["name"] = "local_ui";
    std::ofstream{fixture.task_project / "features/local.json"} << feature.dump(2);
    std::ofstream{fixture.task_project / "lighting.json"} << fixture.path_resolver.loadText("engine://render_fragments/deferred_lighting.json");
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

TEST_CASE("RC-5 A2 recipes preserve typed IR frame plans physical plans and old packages", "[rendercompiler][rc5][rc5-a2]") {
    const auto *directory = std::getenv("RC5_BASELINE_DIR");
    if (!directory || !*directory) SKIP("RC5_BASELINE_DIR is required for the fixed HEAD comparison");
    for (const bool lit : {false, true}) for (const bool hdr : {false, true})
    for (const bool extras : {false, true}) for (const bool xr : {false, true}) {
#if !PELICAN_WITH_OPENXR
        if (xr) continue;
#endif
        CAPTURE(lit, hdr, extras, xr);
        const auto old_root = rc5Document(lit, hdr, extras);
        const auto old_result = rc5Compile(old_root, xr);
        const auto original = rc5Snapshot(old_result);
        REQUIRE(original == readJsonFixture(std::filesystem::path{directory} / rc5Filename(lit, hdr, extras, xr)));
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
}
