namespace {
Rc5Json rc5sIndependentDocument() {
    return TestSupport::renderGraphRoot(Rc5Json{
        {"render_targets", Rc5Json::array({
            {{"name", "independent"}, {"extent_scale", 1.0}, {"format", "B8G8R8A8_UNORM"},
             {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}})},
        {"rendering_passes", Rc5Json::array({{{"name", "main"}, {"passes", Rc5Json::array({
            {{"name", "scene"}, {"type", "fullscreen"},
             {"output", {{"color", "independent"}, {"depth", nullptr}}},
             {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://output_transform"}}}},
            {{"name", "ldr"}, {"type", "fullscreen"},
             {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
             {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://output_transform"}}}}
        })}}})}});
}
}

TEST_CASE("RC-5s ordinary Place constrains only its specified side", "[rendercompiler][rc5s][review-place]") {
    auto root = rc5sIndependentDocument();
    auto &passes = root["rendering"]["rendering_passes"][0]["passes"];
    passes[0]["after"] = {"ldr"};
    auto feature = Rc5Json::parse(R"({"schema":"pelican.render_feature","version":1,"name":"placed",
        "passes":[{"insert":"before:ldr","pass":{"name":"F","type":"fullscreen",
        "output":{"color":"swapchain","depth":null},
        "shader":{"vertex":"engine://fullscreen","fragment":"engine://passthrough"}}}]})");
    root["rendering"]["features"] = {"project://features/local.json"};
    const auto compiled = rc5Compile(root, false, &feature);
    CHECK(framePlanOrder(compiled.variants[0].frame_plans.at("main")) ==
        std::vector<std::string>{"F", "ldr", "scene", "output_transform"});
}

TEST_CASE("RC-5s external preset and fragment refuse removed authoring with their pointer", "[rendercompiler][rc5s][review-boundary]") {
    for (const bool fragment : {false, true}) for (const bool insert : {false, true}) {
        auto config = rc5sIndependentDocument().at("rendering");
        auto &pass = config["rendering_passes"][0]["passes"][0];
        pass[insert ? "insert" : "canonical_anchor"] = insert ? "after:post_ldr" : "post_ldr";
        // A resource with the former anchor name is not a pass that can make this placement valid.
        config["render_targets"].push_back({{"name", "post_ldr"},
            {"type", {{"semantic", "pelican.render.color_signal@1"}, {"arguments", nlohmann::json::object()}}}});
        const auto external = fragment
            ? Rc5Json{{"schema", "pelican.render_fragment"}, {"version", 1}, {"name", "external"},
                {"render_targets", config.at("render_targets")}, {"passes", config["rendering_passes"][0]["passes"]}}
            : Rc5Json{{"schema", "pelican.render_pipeline"}, {"version", 1}, {"name", "external"}, {"config", config}};
        const auto preset = fragment
            ? Rc5Json{{"schema", "pelican.render_pipeline"}, {"version", 2}, {"name", "recipe"},
                {"config", {{"base", Rc5Json::array({{{"family", "main"}, {"fragments", {"fixture://fragment"}}}})}}}}
            : external;
        try {
            (void)resolveRenderPipelinePreset(Rc5Json{{"pipeline", {{"preset", "fixture://preset"}}}},
                [&](std::string_view ref) { return (ref == "fixture://fragment" ? external : preset).dump(); });
            FAIL("removed external authoring was accepted");
        } catch (const GraphRegistryError &error) {
            CHECK(error.code() == "render_graph.canonical_anchor_removed@1");
            CHECK(error.detail().at("document") == (fragment ? "fixture://fragment" : "fixture://preset"));
            CHECK(error.detail().at("pointer") == std::string(fragment ? "/passes/0/" : "/config/rendering_passes/0/passes/0/") +
                (insert ? "insert" : "canonical_anchor"));
        }
    }
}

TEST_CASE("RC-5s every offered snapshot candidate passes the final compiler", "[rendercompiler][rc5s][review-snapshot]") {
    for (auto root : {rc5sIndependentDocument(), rc5Document(false, false, false)}) {
        const auto load = [](std::string_view ref) { return PathResolver{}.loadText(ref); };
        const auto composed = composeRenderFeatureConfig(root, RenderFeatureComposeDependencies{load, true, {}, load});
        const auto candidates = renderSnapshotAfterCandidates(composed.config);
        if (root["rendering"].contains("pipeline")) {
            // The hybrid display is first produced after transparency, outside v1's snapshot region.
            CHECK(candidates.empty());
        } else REQUIRE_FALSE(candidates.empty());
        for (const auto &candidate : candidates) {
            CAPTURE(candidate);
            auto with_snapshot = root;
            with_snapshot["rendering"]["snapshots"] = Rc5Json::array({{{"name", "snap"}, {"after", candidate}}});
            CHECK_NOTHROW(rc5Compile(with_snapshot, false));
        }
    }
}

TEST_CASE("RC-5s real pelican_ui fullscreen and its dependent survive XR selection", "[rendercompiler][rc5s][graph-variant]") {
    auto pass = Rc5Json{{"name", "pelican_ui"}, {"type", "fullscreen"}};
    const auto policy = compileGraphVariantPolicy(GraphVariantPolicyRequest{RenderPipelineGraphVariant::preview});
    GraphVariantFeatureSelection selection{policy};
    auto plain = Rc5Json{{"passes", Rc5Json::array({{{"pass", pass}, {"insert", "end"}}})}};
    CHECK(selection.decide("ordinary", plain).disposition == GraphVariantFeatureDisposition::include);
    plain["passes"][0]["pass"]["name"] = "dependent";
    plain["passes"][0]["insert"] = "after:pelican_ui";
    CHECK(selection.decide("dependent", plain).disposition == GraphVariantFeatureDisposition::include);
#if PELICAN_WITH_OPENXR
    GraphVariantFeatureSelection xr{compileGraphVariantPolicy(GraphVariantPolicyRequest{RenderPipelineGraphVariant::xr})};
    CHECK(xr.decide("dependent", plain).disposition == GraphVariantFeatureDisposition::include);
#endif
    for (const auto variant : {RenderPipelineGraphVariant::flat, RenderPipelineGraphVariant::preview,
                               RenderPipelineGraphVariant::xr}) {
#if !PELICAN_WITH_OPENXR
        if (variant == RenderPipelineGraphVariant::xr) continue;
#endif
        ProgramInputFixture fixture;
        fixture.config = rc5sIndependentDocument();
        auto &passes = fixture.config["rendering"]["rendering_passes"][0]["passes"];
        passes[0]["name"] = "pelican_ui";
        auto dependent = passes[1];
        dependent["name"] = "dependent";
        dependent["after"] = {"pelican_ui", "ldr"};
        passes.push_back(dependent);
        fixture.backend_context = VulkanRenderCompilerBackendContext{vk::Format::eB8G8R8A8Srgb,
            vk::Extent2D{64, 64}, {}, {}, {}, VulkanRenderCompilerDevicePlanningMode::compiler_only};
        fixture.variants = {{.graph_variant = variant,
            .artifact = variant == RenderPipelineGraphVariant::preview ? RenderCompilerProgramArtifact::data_only
                                                                      : RenderCompilerProgramArtifact::runtime_package}};
        const auto result = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), fixture.input());
        const auto &nodes = result.variants[0].render_graph->graphs[0].nodes;
        for (const auto *name : {"pelican_ui", "dependent"})
            CHECK(std::any_of(nodes.begin(), nodes.end(), [&](const auto &node) { return node.name == name; }));
    }
}

TEST_CASE("RC-5s sprite resource uses induce incoming and outgoing barriers", "[rendercompiler][rc5s][rc5s-sprite-barriers]") {
    auto root = rc5Document(false, false, false);
    root["rendering"]["features"] = {"engine://features/sprite.json"};
    const auto load = [](std::string_view reference) { return PathResolver{}.loadText(reference); };
    const auto composed = composeRenderFeatureConfig(root, RenderFeatureComposeDependencies{load, true, {}, load});
    const auto ir = TestSupport::renderGraphOf(composed.config);
    const auto &graph = ir.graphs.at(0);
    const auto sprite = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const auto &node) { return node.name == "sprite_draw"; });
    REQUIRE(sprite != graph.nodes.end());
    CHECK(sprite->uses.size() == 2);
    const auto frame = planRenderGraph(ir, graph);
    const auto planned = std::find_if(frame.nodes.begin(), frame.nodes.end(), [](const auto &node) { return node.name == "sprite_draw"; });
    REQUIRE(planned != frame.nodes.end());
    CHECK(planned->reads == std::vector<std::string>{"lit_color", "scene_depth"});
    CHECK(planned->writes == std::vector<std::string>{"lit_color", "scene_depth"});
    for (const auto *resource : {"lit_color", "scene_depth"}) {
        CHECK(std::any_of(frame.barriers.begin(), frame.barriers.end(), [&](const auto &b) {
            return b.from == "forward_transparent" && b.to == "sprite_draw" && b.resource == resource;
        }));
    }
    CHECK(std::any_of(frame.barriers.begin(), frame.barriers.end(), [](const auto &b) {
        return b.from == "sprite_draw" && b.to == "scene_present" && b.resource == "lit_color";
    }));
}

TEST_CASE("RC-5s independent scene and display passes can reverse order", "[rendercompiler][rc5s][rc5s-a2]") {
    auto root = rc5sIndependentDocument();
    root["rendering"]["frame_node_order"] = {{"main", {"ldr", "scene"}}};
    const auto result = rc5Compile(root, false);
    const auto &variant = result.variants.at(0);
    const auto order = framePlanOrder(variant.frame_plans.at("main"));
    REQUIRE(order == std::vector<std::string>{"ldr", "scene", "output_transform"});
    const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package);
    CHECK(physicalNodeOrderOf(physical.target_plans.at("main")->scopes).nodes == order);
    for (const auto &node : variant.render_graph->graphs.at(0).nodes)
        CHECK(node.name.rfind("__anchor_", 0) != 0);

    root["rendering"]["rendering_passes"][0]["passes"][1]["input"] = {"independent"};
    REQUIRE_THROWS_AS(rc5Compile(root, false), GraphRegistryError);
}

TEST_CASE("RC-5s saved anchor ordering entries warn and preserve the remaining order", "[rendercompiler][rc5s][rc5s-a5]") {
    auto root = rc5sIndependentDocument();
    root["rendering"]["frame_node_order"] = {{"main", {"ldr", "__anchor_sprite", "scene"}}};
    const auto result = rc5Compile(root, false);
    const auto &variant = result.variants.at(0);
    CHECK(framePlanOrder(variant.frame_plans.at("main")) == std::vector<std::string>{"ldr", "scene", "output_transform"});
    const auto &diagnostics = variant.compiled_pipeline->diagnostics;
    const auto warning = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto &d) {
        return d.kind == RenderPipelineDiagnosticKind::anchor_ignored;
    });
    REQUIRE(warning != diagnostics.end());
    CHECK(warning->subject == "__anchor_sprite");
    CHECK(warning->detail_fields.at("severity") == "warning");
}

TEST_CASE("RC-5s removed authored canonical field identifies its JSON pointer", "[rendercompiler][rc5s][rc5s-a5]") {
    auto root = rc5sIndependentDocument();
    root["rendering"]["rendering_passes"][0]["passes"][0]["canonical_anchor"] = "post_ldr";
    try {
        (void)rc5Compile(root, false);
        FAIL("removed canonical_anchor was accepted");
    } catch (const GraphRegistryError &error) {
        CHECK(error.code() == "render_graph.canonical_anchor_removed@1");
        CHECK(error.detail().at("pointer") == "/rendering/rendering_passes/0/passes/0/canonical_anchor");
        CHECK(error.detail().contains("suggestion"));
    }
}

TEST_CASE("RC-5s snapshot uses a concrete pass and retains the one snapshot limit", "[rendercompiler][rc5s][snapshot]") {
    auto root = rc5sIndependentDocument();
    root["rendering"]["snapshots"] = Rc5Json::array({{{"name", "capture"}, {"after", "ldr"}}});
    const auto result = rc5Compile(root, false);
    const auto order = framePlanOrder(result.variants.at(0).frame_plans.at("main"));
    CHECK(std::find(order.begin(), order.end(), "__snapshot_capture") != order.end());
    root["rendering"]["snapshots"].push_back({{"name", "second"}, {"after", "ldr"}});
    REQUIRE_THROWS_AS(rc5Compile(root, false), std::exception);
    root["rendering"]["snapshots"].erase(1);
    root["rendering"]["snapshots"][0]["after"] = "post_ldr";
    try {
        (void)rc5Compile(root, false);
        FAIL("removed snapshot anchor was accepted");
    } catch (const GraphRegistryError &error) {
        CHECK(error.code() == "render_graph.canonical_anchor_removed@1");
        CHECK(error.detail().at("pointer") == "/snapshots/0/after");
    }
}

TEST_CASE("RC-5s old insert names reject only when no concrete pass has that name", "[rendercompiler][rc5s][rc5s-a5]") {
    auto root = rc5sIndependentDocument();
    root["rendering"]["features"] = {"project://features/local.json"};
    Rc5Json feature{{"schema", "pelican.render_feature"}, {"version", 1}, {"name", "local_ui"},
        {"passes", Rc5Json::array({{{"insert", "after:post_ldr"}, {"pass", {
            {"name", "overlay"}, {"type", "ui"},
            {"output", {{"color", "swapchain"}, {"depth", nullptr}}}, {"color_load_op", "load"}}}}})}};
    try {
        (void)rc5Compile(root, false, &feature);
        FAIL("removed insert anchor was accepted");
    } catch (const GraphRegistryError &error) {
        CHECK(error.code() == "render_graph.canonical_anchor_removed@1");
        CHECK(error.detail().contains("pointer"));
        CHECK(error.detail().at("anchor") == "post_ldr");
    }
    root["rendering"]["rendering_passes"][0]["passes"][1]["name"] = "post_ldr";
    const auto reloaded = Rc5Json::parse(root.dump());
    const auto result = rc5Compile(reloaded, false, &feature);
    CHECK(framePlanOrder(result.variants.at(0).frame_plans.at("main")) ==
          std::vector<std::string>{"scene", "post_ldr", "overlay", "output_transform"});
}

TEST_CASE("RC-5s output terminal waits on graph compute as well as rendering", "[rendercompiler][rc5s][output-terminal]") {
    auto root = rc5sIndependentDocument();
    root["nodes"] = Rc5Json::array({fg1Image("tail"), fg1Fill("delayed", "tail")});
    root["nodes"][1]["parameters"]["pass_set"] = "main";
    root["nodes"][1]["parameters"]["after"] = {"scene"};
    const auto result = rc5Compile(root, false);
    const auto order = framePlanOrder(result.variants.at(0).frame_plans.at("main"));
    CHECK(order.back() == "output_transform");
    CHECK(std::find(order.begin(), order.end(), "delayed") < std::prev(order.end()));
}

TEST_CASE("RC-5s graph insert refusal points to the authored node array entry", "[rendercompiler][rc5s][rc5s-a5]") {
    auto root = rc5sIndependentDocument();
    auto fill = fg1Fill("placed", "probe_image");
    fill["parameters"]["pass_set"] = "main";
    fill["parameters"]["insert"] = {{"after", "post_ldr"}};
    root["nodes"] = Rc5Json::array({fg1Image("probe_image"), fill});
    try {
        (void)rc5Compile(root, false);
        FAIL("removed graph insert anchor was accepted");
    } catch (const GraphRegistryError &error) {
        CHECK(error.code() == "render_graph.canonical_anchor_removed@1");
        CHECK(error.detail().at("pointer") == "/nodes/1/parameters/insert");
        CHECK(root.at(Rc5Json::json_pointer{error.detail().at("pointer").get<std::string>()}) == fill["parameters"]["insert"]);
    }
    root["rendering"]["rendering_passes"][0]["passes"][1]["name"] = "post_ldr";
    CHECK_NOTHROW(rc5Compile(Rc5Json::parse(root.dump()), false));
}

TEST_CASE("RC-5s rewritten engine features preserve flat preview and XR selection", "[rendercompiler][rc5s][graph-variant]") {
    const auto legacy = readJsonFixture(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test/fixtures/rc5s_legacy_features.json");
    std::size_t variants = 0;
    for (const auto variant : {RenderPipelineGraphVariant::flat, RenderPipelineGraphVariant::preview, RenderPipelineGraphVariant::xr}) {
#if !PELICAN_WITH_OPENXR
        if (variant == RenderPipelineGraphVariant::xr) continue;
#endif
        const auto policy = compileGraphVariantPolicy(GraphVariantPolicyRequest{variant});
        GraphVariantFeatureSelection selection{policy};
        for (const auto &[name, old_document] : legacy.items()) {
            CAPTURE(name, variant);
            const auto current = Rc5Json::parse(PathResolver{}.loadText("engine://features/" + name + ".json"));
            const auto expected = decideGraphVariantFeature(policy, name, old_document);
            const auto actual = selection.decide(name, current);
            CHECK(actual.disposition == expected.disposition);
        }
        ++variants;
    }
#if PELICAN_WITH_OPENXR
    CHECK(variants == 3);
#else
    CHECK(variants == 2);
#endif
}

TEST_CASE("RC-5s hybrid HDR sprite uses authored bindings", "[rendercompiler][rc5s][sprite]") {
    auto root = rc5Document(false, true, false);
    root["rendering"]["features"].push_back({{"ref", "engine://features/sprite.json"},
        {"parameters", {{"color", "lit_color"}, {"depth", "scene_depth"}}}});
    const auto result = rc5Compile(root, false);
    const auto &graph = result.variants.at(0).render_graph->graphs.at(0);
    const auto sprite = std::find_if(graph.nodes.begin(), graph.nodes.end(), [](const auto &node) { return node.name == "sprite_draw"; });
    REQUIRE(sprite != graph.nodes.end());
    REQUIRE(sprite->uses.size() == 2);
    CHECK(sprite->uses[0].resource == "lit_color");
    CHECK(sprite->uses[1].resource == "scene_depth");
    const auto order = framePlanOrder(result.variants.at(0).frame_plans.at("main_render"));
    CHECK(std::find(order.begin(), order.end(), "sprite_draw") < std::find(order.begin(), order.end(), "scene_present"));
    CHECK(std::find(order.begin(), order.end(), "scene_present") < std::find(order.begin(), order.end(), "hdr_tonemap"));
}

TEST_CASE("RC-5s frozen v0 sprite requires explicit target bindings", "[rendercompiler][rc5s][sprite]") {
    const auto legacy = readJsonFixture(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "test/fixtures/project_corpus/v0/sprite_demo/passes/main.json");
    const auto load = [](std::string_view reference) { return PathResolver{}.loadText(reference); };
    try {
        (void)composeRenderFeatureConfig(legacy, RenderFeatureComposeDependencies{load, true, {}, load});
        FAIL("v0 direct sprite silently acquired inferred bindings");
    } catch (const GraphRegistryError &error) {
        CHECK(error.code() == "feature.parameter_target_unknown@1");
        CHECK(error.detail().at("target") == "lit_color");
        CHECK(error.detail().at("feature") == "sprite");
    }
}

TEST_CASE("RC-5s all eight shipping projects have no anchor in IR frame or physical plans", "[rendercompiler][rc5s][rc5s-a1]") {
    const vk::ApplicationInfo application{"rc5s-plan-device", 1, "pelican", 1, VK_API_VERSION_1_3};
    const auto instance = vk::createInstanceUnique(vk::InstanceCreateInfo{{}, &application});
    const auto devices = instance->enumeratePhysicalDevices();
    REQUIRE_FALSE(devices.empty());
    const std::vector<std::pair<std::string, std::string>> projects{
        {"animgraph_demo", "passes/main.json"}, {"example", "passes/main_rendering_config.json"},
        {"pathtrace_demo", "rendering.json"}, {"posegraph_demo", "passes/main.json"},
        {"prefab_demo", "passes/main.json"}, {"raytracing_demo", "rendering.json"},
        {"sprite_demo", "passes/main.json"}, {"vrm_xr_demo", "passes/main.json"}};
    std::size_t compiled = 0;
    for (const auto &[project, document] : projects) {
        CAPTURE(project);
        ProgramInputFixture fixture;
        const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / project;
        fixture.path_resolver.setup(root, false);
        fixture.config = readJsonFixture(root / document);
        fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
        fixture.backend_context = VulkanRenderCompilerBackendContext{
            vk::Format::eB8G8R8A8Srgb, vk::Extent2D{160, 90}, {}, {}, {},
            VulkanRenderCompilerDevicePlanningMode::compiler_only};
        fixture.backend_context.runtime_capabilities.ray_tracing_pipeline = true;
        fixture.backend_context.runtime_capabilities.ray_query = true;
        fixture.backend_context.runtime_capabilities.ray_material_bindless = true;
        fixture.backend_context.physical_device = devices.front();
        fixture.backend_context.device_planning_mode = VulkanRenderCompilerDevicePlanningMode::device_required;
        auto input = fixture.input();
        input.runtime_shader_compiler_enabled = true;
        input.ray_tracing_project = loadRayTracingProjectConfig(fixture.config,
            [&](std::string_view reference) { return fixture.path_resolver.loadText(reference); }, true);
        const auto result = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
        for (const auto &graph : result.variants.at(0).render_graph->graphs)
            for (const auto &node : graph.nodes) CHECK_FALSE(node.name.starts_with("__anchor_"));
        const auto snapshot = rc5Snapshot(result);
        for (const auto &[name, frame] : snapshot.at("frame_plans").items())
            for (const auto &node : frame.at("nodes")) CHECK_FALSE(node.at("name").get<std::string>().starts_with("__anchor_"));
        for (const auto &[name, physical] : snapshot.at("physical_plans").items())
            for (const auto &scope : physical.at("scopes"))
                for (const auto &node : scope.at("nodes")) CHECK_FALSE(node.get<std::string>().starts_with("__anchor_"));
        ++compiled;
    }
    CHECK(compiled == 8);
}
