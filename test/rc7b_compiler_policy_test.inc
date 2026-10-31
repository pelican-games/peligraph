TEST_CASE("RC-7b default compiler discovers retain-free variant contracts", "[rendercompiler][rc7b][rc7b-generation]") {
#if PELICAN_WITH_OPENXR
    using Json = nlohmann::json;
    const auto compile = [&](const std::string &strength) {
        ProgramInputFixture fixture;
        Json config{{"features", {"project://rc7b-variant.json"}},
            {"render_targets", Json::array({{{"name", "sink"}, {"format", "R8_UNORM"},
                {"format_class", "data"}, {"extent_scale", 1.0}, {"usage", {"COLOR_ATTACHMENT"}}}})},
            {"rendering_passes", Json::array({{{"name", "main"}, {"passes", Json::array()}}})}};
        auto &passes = config["rendering_passes"][0]["passes"];
        for (const auto *letter : {"a", "b"}) {
            const auto resource = std::string{"temporary_"} + letter;
            const auto write = std::string{"write_"} + letter;
            const auto read = std::string{"read_"} + letter;
            Json producer{{"name", write}, {"type", "fullscreen"},
                {"output", {{"color", resource}, {"depth", nullptr}}},
                {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}};
            if (resource == "temporary_b") producer["after"] = {"read_a"};
            passes.push_back(producer);
            passes.push_back({{"name", read}, {"type", "fullscreen"}, {"input", {resource}},
                {"output", {{"color", resource == "temporary_a" ? "sink" : "swapchain"}, {"depth", nullptr}}},
                {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}});
        }
        if (strength != "automatic") for (const auto *name : {"temporary_a", "temporary_b"})
            config["target_planning"]["graphs"]["main"]["resources"][name]["alias_group"] = {{"value", "pair"}, {"strength", strength}};
        REQUIRE(config.dump().find("retain") == std::string::npos);
        fixture.useAuthoredConfig(config, "rc7b-variants");
        fixture.backend_context = VulkanRenderCompilerBackendContext{vk::Format::eB8G8R8A8Srgb,
            {64, 64}, {}, {}, {}, VulkanRenderCompilerDevicePlanningMode::compiler_only};
        fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}, {.graph_variant = RenderPipelineGraphVariant::xr}};
        fixture.backend_context.planning_facts = TestSupport::rc7cDeviceFacts();
    auto input = fixture.input();
        input.runtime_shader_compiler_enabled = true;
        std::size_t reads = 0;
        // Controlled variant source facts: the default program resolves flat
        // then XR. Each IR is asserted below, independently of alias outcomes.
        input.load_document = [&](std::string_view ref) {
            if (ref != "project://rc7b-variant.json") return fixture.path_resolver.loadText(ref);
            Json feature{{"schema", "pelican.render_feature"}, {"version", 1}, {"name", "rc7b_variant"},
                {"render_targets", Json::array()}};
            ++reads;
            for (const auto *name : {"temporary_a", "temporary_b"}) {
                auto usage = Json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"});
                if (reads == 2 && std::string_view{name} == "temporary_b") usage.push_back("STORAGE");
                feature["render_targets"].push_back({{"name", name}, {"format", "R8G8B8A8_UNORM"},
                    {"format_class", "data"}, {"extent_scale", 1.0}, {"usage", usage}});
            }
            return feature.dump();
        };
        auto result = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
        REQUIRE(reads == 2);
        REQUIRE(result.variants.size() == 2);
        const auto image = [](const auto &variant) -> const ImageResource & {
            const auto *found = findConfigImage(*variant.render_graph, "temporary_b"); REQUIRE(found); return *found;
        };
        CHECK_FALSE(bool(image(result.variants[0]).usage & ImageUsageBit::storage));
        CHECK(bool(image(result.variants[1]).usage & ImageUsageBit::storage));
        for (const auto &variant : result.variants) {
            const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package);
            for (const auto &[name, plan] : physical.target_plans) {
                CHECK(plan->alias_groups.empty());
                // The merged usage itself rules out this pair; provisional
                // discovery placements must not become permanent exclusions.
                CHECK(plan->generation_alias_exclusions.empty());
                CHECK(std::any_of(plan->decisions.begin(), plan->decisions.end(), [](const auto &d) {
                    return d.id == "pelican.plan.alias_pair_ineligible@1" &&
                        d.subject == "temporary_a/temporary_b" && d.selected == "memory_key";
                }));
                CHECK(bool(plan->effective_image_usage.at("temporary_b") & ImageUsageBit::storage));
                if (strength == "prefer") CHECK(std::any_of(plan->policy.begin(), plan->policy.end(), [](const auto &p) {
                    return p.key == "alias_group" && p.status == PlanningPolicyStatus::infeasible_prefer;
                }));
            }
        }
        CHECK_FALSE(result.generation_resources.findImage("temporary_a")->alias_group);
        CHECK_FALSE(result.generation_resources.findImage("temporary_b")->alias_group);
    };
    compile("automatic"); compile("prefer");
    try { compile("require"); FAIL("incompatible required generation alias was accepted"); }
    catch (const GraphRegistryError &error) {
        CHECK(error.code() == "target_planning.requirement_unsatisfied@1");
        CHECK(error.detail().dump().find("temporary_b") != std::string::npos);
    }
#else
    SKIP("XR variants unavailable");
#endif
}

TEST_CASE("RC-7b merged usage restores legal variant sharing", "[rendercompiler][rc7b][rc7b-fix-variants]") {
#if PELICAN_WITH_OPENXR
    using Json = nlohmann::json;
    for (const auto *strength : {"automatic", "prefer", "require"}) {
        DYNAMIC_SECTION(strength) {
        ProgramInputFixture fixture;
        Json config{{"features", {"project://rc7b-usage.json"}},
            {"render_targets", Json::array({{{"name", "sink"}, {"format", "R8_UNORM"},
                {"format_class", "data"}, {"extent_scale", 1.0}, {"usage", {"COLOR_ATTACHMENT"}}}})},
            {"rendering_passes", Json::array({{{"name", "main"}, {"passes", Json::array()}}})}};
        auto &passes = config["rendering_passes"][0]["passes"];
        for (const auto *letter : {"a", "b"}) {
            const auto resource = std::string{"temporary_"} + letter;
            Json writer{{"name", std::string{"write_"} + letter}, {"type", "fullscreen"},
                {"output", {{"color", resource}, {"depth", nullptr}}},
                {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}};
            if (resource == "temporary_b") writer["after"] = {"read_a"};
            passes.push_back(writer);
            passes.push_back({{"name", std::string{"read_"} + letter}, {"type", "fullscreen"}, {"input", {resource}},
                {"output", {{"color", resource == "temporary_a" ? "sink" : "swapchain"}, {"depth", nullptr}}},
                {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}});
        }
        if (std::string_view{strength} != "automatic") for (const auto *name : {"temporary_a", "temporary_b"})
            config["target_planning"]["graphs"]["main"]["resources"][name]["alias_group"] = {{"value", "pair"}, {"strength", strength}};
        fixture.useAuthoredConfig(config, "rc7b-merged-usage");
        fixture.backend_context = VulkanRenderCompilerBackendContext{vk::Format::eB8G8R8A8Srgb,
            {64, 64}, {}, {}, {}, VulkanRenderCompilerDevicePlanningMode::compiler_only};
        fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}, {.graph_variant = RenderPipelineGraphVariant::xr}};
        fixture.backend_context.planning_facts = TestSupport::rc7cDeviceFacts();
    auto input = fixture.input();
        input.runtime_shader_compiler_enabled = true;
        std::size_t reads = 0;
        input.load_document = [&](std::string_view ref) {
            if (ref != "project://rc7b-usage.json") return fixture.path_resolver.loadText(ref);
            Json feature{{"schema", "pelican.render_feature"}, {"version", 1}, {"name", "rc7b_usage"},
                {"render_targets", Json::array()}};
            ++reads;
            for (const auto *name : {"temporary_a", "temporary_b"}) {
                auto usage = Json::array({"COLOR_ATTACHMENT", "SAMPLED"});
                if (reads == 2 || std::string_view{name} == "temporary_b") usage.push_back("STORAGE");
                feature["render_targets"].push_back({{"name", name}, {"format", "R8G8B8A8_UNORM"},
                    {"format_class", "data"}, {"extent_scale", 1.0}, {"usage", usage}});
            }
            return feature.dump();
        };
        const auto result = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
        REQUIRE(reads == 2);
        REQUIRE(result.variants.size() == 2);
        for (std::size_t i = 0; i < result.variants.size(); ++i) {
            const auto &variant = result.variants[i];
            CHECK(bool(findConfigImage(*variant.render_graph, "temporary_a")->usage & ImageUsageBit::storage) == (i == 1));
            CHECK(bool(findConfigImage(*variant.render_graph, "temporary_b")->usage & ImageUsageBit::storage));
            const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package);
            REQUIRE(physical.target_plans.size() == 1);
            const auto &plan = *physical.target_plans.begin()->second;
            REQUIRE(plan.alias_groups.size() == 1);
            CHECK(plan.alias_groups[0].resources == std::vector<std::string>{"temporary_a", "temporary_b"});
            CHECK(plan.generation_alias_exclusions.empty());
            CHECK(plan.effective_image_usage.at("temporary_a") == plan.effective_image_usage.at("temporary_b"));
            const auto dump = vulkanTargetPlanToJson(plan);
            CHECK(dump.at("alias_groups").size() == 1);
            CHECK(std::any_of(plan.decisions.begin(), plan.decisions.end(), [](const auto &d) {
                return d.id == "pelican.plan.alias_group_selected@1";
            }));
            for (const auto &entry : plan.policy) if (entry.key == "alias_group") {
                CHECK(entry.status == PlanningPolicyStatus::adopted);
                if (entry.strength == PlanningStrength::require) CHECK(entry.satisfied == true);
            }
        }
        const auto *a = result.generation_resources.findImage("temporary_a");
        const auto *b = result.generation_resources.findImage("temporary_b");
        REQUIRE(a); REQUIRE(b); REQUIRE(a->alias_group);
        CHECK(a->alias_group == b->alias_group);
        CHECK(result.generation_resources.alias_rejections.empty());
        }
    }
#else
    SKIP("XR variants unavailable");
#endif
}

TEST_CASE("RC-7b final generation accepts required sharing after no_alias drops a prefer peer", "[rendercompiler][rc7b][rc7b-fix-retry]") {
    using Json = nlohmann::json;
    for (const bool retain_b : {false, true}) {
        DYNAMIC_SECTION((retain_b ? "retain" : "no_alias")) {
            ProgramInputFixture fixture;
            Json config{{"render_targets", Json::array()},
                {"rendering_passes", Json::array({{{"name", "main"}, {"passes", Json::array()}}})}};
            auto &passes = config["rendering_passes"][0]["passes"];
            for (const auto *letter : {"a", "b", "c"}) {
                const auto resource = std::string{"temporary_"} + letter;
                config["render_targets"].push_back({{"name", resource}, {"format", "R8G8B8A8_UNORM"},
                    {"format_class", "data"}, {"extent_scale", 1.0},
                    {"usage", {"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"}}});
                Json writer{{"name", std::string{"write_"} + letter}, {"type", "fullscreen"},
                    {"output", {{"color", resource}, {"depth", nullptr}}},
                    {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}};
                if (resource == "temporary_b") writer["after"] = {"read_a"};
                if (resource == "temporary_c") writer["after"] = {"read_b"};
                passes.push_back(writer);
                passes.push_back({{"name", std::string{"read_"} + letter}, {"type", "fullscreen"}, {"input", {resource}},
                    {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
                    {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}});
                config["target_planning"]["graphs"]["main"]["resources"][resource]["alias_group"] =
                    {{"value", "pair"}, {"strength", resource == "temporary_b" ? "prefer" : "require"}};
            }
            config["target_planning"]["graphs"]["main"]["resources"]["temporary_b"]
                [retain_b ? "retain" : "no_alias"] = true;
            if (!retain_b) REQUIRE(config.dump().find("retain") == std::string::npos);
            fixture.useAuthoredConfig(config, "rc7b-retry");
            fixture.backend_context = VulkanRenderCompilerBackendContext{vk::Format::eB8G8R8A8Srgb,
                {64, 64}, {}, {}, {}, VulkanRenderCompilerDevicePlanningMode::compiler_only};
            fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
            fixture.backend_context.planning_facts = TestSupport::rc7cDeviceFacts();
    auto input = fixture.input();
            input.runtime_shader_compiler_enabled = true;
            const auto result = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
            REQUIRE(result.variants.size() == 1);
            const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*result.variants[0].physical_package);
            REQUIRE(physical.target_plans.size() == 1);
            const auto &plan = *physical.target_plans.at("main");
            REQUIRE(plan.alias_groups.size() == 1);
            CHECK(plan.alias_groups[0].resources == std::vector<std::string>{"temporary_a", "temporary_c"});
            const auto *a = result.generation_resources.findImage("temporary_a");
            const auto *b = result.generation_resources.findImage("temporary_b");
            const auto *c = result.generation_resources.findImage("temporary_c");
            REQUIRE(a); REQUIRE(b); REQUIRE(c); REQUIRE(a->alias_group);
            CHECK(a->alias_group == c->alias_group);
            CHECK_FALSE(b->alias_group);
            const auto entry = std::find_if(plan.policy.begin(), plan.policy.end(), [](const auto &p) {
                return p.target == "temporary_b" && p.key == "alias_group";
            });
            REQUIRE(entry != plan.policy.end());
            CHECK(entry->status == PlanningPolicyStatus::infeasible_prefer);
            CHECK(entry->satisfied != true);
            const auto dump = vulkanTargetPlanToJson(plan);
            const auto &policy = dump.at("policy");
            CHECK(std::any_of(policy.begin(), policy.end(), [](const auto &p) {
                return p.at("target") == "temporary_b" && p.at("key") == "alias_group" &&
                    p.at("status") == "infeasible_prefer";
            }));
        }
    }
}

TEST_CASE("RC-7b graph overlap releases generation sharing and its policy display", "[rendercompiler][rc7b][rc7b-fix-graphs]") {
    using Json = nlohmann::json;
    for (const auto *strength : {"automatic", "prefer", "require"}) {
        INFO(strength);
        Json targets = Json::array();
        for (const auto *name : {"temporary_a", "temporary_b", "sink"})
            targets.push_back({{"name", name}, {"format", std::string_view{name} == "sink" ? "R8_UNORM" : "R8G8B8A8_UNORM"},
                {"format_class", "data"}, {"extent_scale", 1.0}, {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}});
        const auto pass = [](const char *name, Json input, const char *output) {
            Json p{{"name", name}, {"type", "fullscreen"},
                {"output", {{"color", output}, {"depth", nullptr}}},
                {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}};
            if (!input.empty()) p["input"] = input;
            return p;
        };
        auto g1 = Json::array({pass("write_a", Json::array(), "temporary_a"),
            pass("read_a", {"temporary_a"}, "sink"), pass("write_b", Json::array(), "temporary_b"),
            pass("read_b", {"temporary_b"}, "swapchain")});
        g1[2]["after"] = {"read_a"};
        auto g2 = Json::array({pass("g2_write_a", Json::array(), "temporary_a"),
            pass("g2_write_b", Json::array(), "temporary_b"), pass("g2_read_both", {"temporary_a", "temporary_b"}, "swapchain")});
        g2[1]["after"] = {"g2_write_a"};
        Json config{{"render_targets", targets}, {"rendering_passes", Json::array({
            {{"name", "G1"}, {"passes", g1}}, {{"name", "G2"}, {"passes", g2}}})}};
        if (std::string_view{strength} != "automatic") for (const auto *name : {"temporary_a", "temporary_b"})
            config["target_planning"]["graphs"]["G1"]["resources"][name]["alias_group"] = {{"value", "pair"}, {"strength", strength}};
        // Exercise the finalized two-graph IR directly: feature composition
        // otherwise adds an unrelated output_transform of the same name.
        auto ir = std::make_shared<const RenderGraphIR>(TestSupport::renderGraphOf(config));
        auto pipeline = std::make_shared<CompiledRenderPipeline>();
        pipeline->target_planning = parseTargetPlanningPolicy(config);
        resolveFinalPlanningPolicy(*pipeline, *ir);
        const auto compile = [&](bool overlap = true) {
            RenderCompilerProgramVariantOutput variant;
            variant.graph_variant = RenderPipelineGraphVariant::flat;
            auto selected_ir = std::make_shared<RenderGraphIR>(*ir);
            if (!overlap) selected_ir->graphs.resize(1);
            variant.render_graph = selected_ir;
            variant.compiled_pipeline = pipeline;
            variant.frame_plans = TestSupport::framePlanMapOf(*selected_ir);
            auto physical = std::make_unique<VulkanRenderCompilerPhysicalPackage>();
            const auto definitions = renderTargetDefinitionsOf(*selected_ir);
            physical->target_plan_compilation = compileRenderingTargetPlans(*selected_ir, variant.frame_plans,
                definitions, {}, vk::Format::eB8G8R8A8Srgb, TestSupport::rc7cDeviceFacts(), {}, {}, pipeline->target_planning);
            physical->image_placements = imagePlacementsOf(definitions, physical->target_plan_compilation);
            for (const auto &plan : physical->target_plan_compilation.plans)
                physical->target_plans.emplace(plan->graph, plan);
            variant.physical_package = std::move(physical);
            return variant;
        };
        if (std::string_view{strength} == "require") {
            // A valid required plan, followed by the lost-placement state of
            // the original bug: no resolver proposal and no rejection entry.
            auto control = compile(false);
            REQUIRE(completeGenerationResources(std::span{&control, 1}, {64, 64})
                .findImage("temporary_a")->alias_group.has_value());
            auto &control_physical = requireVulkanRenderCompilerPhysicalPackage(*control.physical_package);
            for (auto &placement : control_physical.image_placements) placement.alias_group.reset();
            REQUIRE(resolveCompilerGenerationResources(std::span{&control, 1}, {64, 64}).alias_rejections.empty());
            try {
                completeGenerationResources(std::span{&control, 1}, {64, 64});
                FAIL_CHECK("final unshared placement satisfied require");
            } catch (const GraphRegistryError &error) {
                CHECK(error.code() == "target_planning.alias_generation_contract@1");
                CHECK(error.detail().at("graph") == "G1");
                CHECK(error.detail().at("resource") == "temporary_a");
                CHECK(error.detail().at("peer") == "temporary_b");
            }
            try {
                auto variant = compile();
                completeGenerationResources(std::span{&variant, 1}, {64, 64});
                FAIL_CHECK("required G1 sharing lost to G2 overlap was accepted");
            } catch (const GraphRegistryError &error) {
                CHECK(error.code() == "target_planning.alias_generation_contract@1");
                CHECK(error.detail().dump().find("G1") != std::string::npos);
                CHECK(error.detail().dump().find("temporary_a") != std::string::npos);
                CHECK(error.detail().dump().find("alias_group") != std::string::npos);
            }
            continue;
        }
        auto variant = compile();
        const auto resources = completeGenerationResources(std::span{&variant, 1}, {64, 64});
        const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package);
        CHECK(resources.findImage("temporary_a")->alias_group != resources.findImage("temporary_b")->alias_group);
        for (const auto &[graph, plan] : physical.target_plans) {
            INFO(graph);
            for(const auto &group:plan->alias_groups)CHECK_FALSE((std::find(group.resources.begin(),group.resources.end(),"temporary_a")!=group.resources.end() && std::find(group.resources.begin(),group.resources.end(),"temporary_b")!=group.resources.end()));
            if(graph=="G1")CHECK(std::any_of(plan->decisions.begin(),plan->decisions.end(),[](const auto &d){return d.id=="pelican.plan.alias_generation_contract@1";}));
            const auto resource = [&](std::string_view name) -> const VulkanPhysicalResourcePlan & {
                const auto found = std::find_if(plan->resources.begin(), plan->resources.end(),
                    [&](const auto &r) { return r.logical_resource == name; });
                REQUIRE(found != plan->resources.end()); return *found;
            };
            const auto &a = resource("temporary_a"), &b = resource("temporary_b");
            CHECK(a.lifetime.used); CHECK(b.lifetime.used);
            if (graph == "G1") CHECK(a.lifetime.last_use < b.lifetime.first_use);
            else {
                CHECK(a.lifetime.first_use <= b.lifetime.last_use);
                CHECK(b.lifetime.first_use <= a.lifetime.last_use);
            }
            if (graph == "G1" && std::string_view{strength} == "prefer") {
                CHECK(std::any_of(plan->policy.begin(), plan->policy.end(), [](const auto &entry) {
                    return entry.key == "alias_group" && entry.status == PlanningPolicyStatus::infeasible_prefer;
                }));
                for (const auto &entry : plan->policy) if (entry.key == "alias_group") {
                    CHECK(entry.satisfied != true);
                }
            }
        }
        CHECK(resources.findImage("temporary_a")->alias_group != resources.findImage("temporary_b")->alias_group);
    }
}
