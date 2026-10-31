TEST_CASE("RC-7c generation usage rebuild drops a retried retain before requirements", "[rendercompiler][rc7c][rc7c-c6]") {
    using Json=nlohmann::json;
    Json config{{"render_targets",Json::array()},
        {"rendering_passes",Json::array({{{"name","main"},{"passes",Json::array()}}})}};
    auto &passes=config["rendering_passes"][0]["passes"];
    for(const auto *letter:{"a","b"}) {
        const auto resource=std::string{"temporary_"}+letter;
        config["render_targets"].push_back({{"name",resource},{"format","R8G8B8A8_UNORM"},{"format_class","data"},
            {"extent_scale",1.0},{"usage",{"COLOR_ATTACHMENT","SAMPLED"}}});
        Json writer{{"name",std::string{"write_"}+letter},{"type","fullscreen"},
            {"output",{{"color",resource},{"depth",nullptr}}},
            {"shader",{{"vertex","engine://fullscreen"},{"fragment","engine://passthrough"}}}};
        if(resource=="temporary_b")writer["after"]={"read_a"};
        passes.push_back(writer);
        passes.push_back({{"name",std::string{"read_"}+letter},{"type","fullscreen"},{"input",{resource}},
            {"output",{{"color","swapchain"},{"depth",nullptr}}},
            {"shader",{{"vertex","engine://fullscreen"},{"fragment","engine://passthrough"}}}});
        config["target_planning"]["graphs"]["main"]["resources"][resource]["alias_group"]="pair";
    }
    config["target_planning"]["graphs"]["main"]["resources"]["temporary_a"]["retain"]={{"value",true},{"strength","prefer"}};
    ProgramInputFixture fixture;
    fixture.useAuthoredConfig(config,"rc7c-retain-retry");
    fixture.backend_context=VulkanRenderCompilerBackendContext{vk::Format::eB8G8R8A8Srgb,{64,64},{},{},{},VulkanRenderCompilerDevicePlanningMode::compiler_only};
    auto facts=TestSupport::rc7cDeviceFacts();
    facts.image_memory_requirements=[](const vk::ImageCreateInfo &info) {
        auto req=TestSupport::rc7cDeviceRequirements(info);
        if(req)req->memory_type_bits=bool(info.usage&vk::ImageUsageFlagBits::eTransferSrc)?2u:1u;
        return req;
    };
    fixture.backend_context.planning_facts=facts;
    fixture.variants={{.graph_variant=RenderPipelineGraphVariant::flat}};
    auto input=fixture.input();input.runtime_shader_compiler_enabled=true;
    const auto result=runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(),input);
    const auto &physical=requireVulkanRenderCompilerPhysicalPackage(*result.variants[0].physical_package);
    const auto &plan=*physical.target_plans.at("main");
    CHECK_FALSE(bool(plan.effective_image_usage.at("temporary_a")&ImageUsageBit::transfer_src));
    CHECK_FALSE(bool(result.generation_resources.findImage("temporary_a")->usage&ImageUsageBit::transfer_src));
    CHECK(std::any_of(plan.policy.begin(),plan.policy.end(),[](const auto &e){return e.key=="retain"&&e.status==PlanningPolicyStatus::infeasible_prefer;}));
    REQUIRE(result.generation_resources.findImage("temporary_a")->alias_group);
    CHECK(result.generation_resources.findImage("temporary_a")->alias_group==result.generation_resources.findImage("temporary_b")->alias_group);
}

TEST_CASE("RC-7c fix runtime layered shadow is excluded before packing", "[rendercompiler][rc7c-fix1]") {
    using Json=nlohmann::json;
    for(const std::string strength:{"automatic","prefer","require"}) {
        CAPTURE(strength);
        Json config{{"features",Json::array({{{"ref","engine://features/shadow_directional.json"},
            {"parameters",{{"resolution",64}}}}})},
            {"render_targets",Json::array({{{"name","T"},{"format","R8G8B8A8_UNORM"},{"format_class","data"},
                {"extent_scale",1.0},{"usage",{"COLOR_ATTACHMENT","SAMPLED"}}}})},
            {"rendering_passes",Json::array({{{"name","main"},{"passes",Json::array({
                {{"name","read_shadow"},{"type","fullscreen"},{"input",{"shadow_map"}},
                    {"output",{{"color","swapchain"},{"depth",nullptr}}},
                    {"shader",{{"vertex","engine://fullscreen"},{"fragment","engine://passthrough"}}}},
                {{"name","write_T"},{"type","fullscreen"},{"after",{"read_shadow"}},
                    {"output",{{"color","T"},{"depth",nullptr}}},
                    {"shader",{{"vertex","engine://fullscreen"},{"fragment","engine://passthrough"}}}},
                {{"name","read_T"},{"type","fullscreen"},{"input",{"T"}},
                    {"output",{{"color","swapchain"},{"depth",nullptr}}},
                    {"shader",{{"vertex","engine://fullscreen"},{"fragment","engine://passthrough"}}}}
            })}}})}};
        if(strength!="automatic")for(const auto *name:{"shadow_map","T"})
            config["target_planning"]["graphs"]["main"]["resources"][name]["alias_group"]={{"value","pair"},{"strength",strength}};
        ProgramInputFixture fixture;fixture.useAuthoredConfig(config,"rc7c-runtime-layers");
        fixture.backend_context=VulkanRenderCompilerBackendContext{vk::Format::eB8G8R8A8Srgb,{64,64},{},{},{},VulkanRenderCompilerDevicePlanningMode::compiler_only};
        fixture.backend_context.planning_facts=TestSupport::rc7cDeviceFacts();
        fixture.variants={{.graph_variant=RenderPipelineGraphVariant::flat}};
        auto input=fixture.input();input.runtime_shader_compiler_enabled=true;
        if(strength=="require") {
            CHECK_THROWS_WITH(runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(),input),
                Catch::Matchers::ContainsSubstring("runtime_array_layers"));
            continue;
        }
        const auto result=runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(),input);
        REQUIRE(result.generation_resources.findImage("shadow_map")->runtime_array_layers);
        const auto &plan=*requireVulkanRenderCompilerPhysicalPackage(*result.variants[0].physical_package).target_plans.at("main");
        for(const auto &region:plan.memory_regions)for(const auto &r:region.residents)CHECK(r.resource!="shadow_map");
        CHECK(plan.alias_groups.empty());
        CHECK_FALSE(result.generation_resources.findImage("shadow_map")->alias_group);
        if(strength=="prefer")CHECK(std::any_of(plan.policy.begin(),plan.policy.end(),[](const auto &e){
            return e.key=="alias_group" && e.status==PlanningPolicyStatus::infeasible_prefer;}));
    }
}

TEST_CASE("RC-7c fix buffer retain is rejected before image capability query", "[rendercompiler][rc7c-fix2]") {
    using Json=nlohmann::json;
    Json config{{"buffers",Json::array({{{"name","B"},{"size",64},{"usage",{"STORAGE"}}}})},
        {"compute_tasks",Json::array({{{"name","write_B"},{"shader","shaders/independent"},
            {"writes",{"B"}},{"dispatch",{{"groups",{1,1,1}}}}}})},
        {"rendering_passes",Json::array({{{"name","main"},{"passes",Json::array({
            {{"name","present"},{"type","fullscreen"},{"after",{"write_B"}},
             {"output",{{"color","swapchain"},{"depth",nullptr}}},
             {"shader",{{"vertex","engine://fullscreen"},{"fragment","engine://passthrough"}}}}
        })}}})}};
    const auto graphs=TestSupport::renderGraphOf(config);
    const auto targets=test_support::renderTargetDefinitionsOf(config);
    for(const std::string strength:{"prefer","require"}) {
        config["target_planning"]["graphs"]["main"]["resources"]["B"]["retain"]={{"value",true},{"strength",strength}};
        const auto compile=[&] {return compileRenderingTargetPlans(graphs,TestSupport::framePlanMapOf(graphs),targets,
            compileSampleCountPolicy(config),vk::Format::eB8G8R8A8Srgb,TestSupport::rc7cDeviceFacts(),
            std::nullopt,std::nullopt,parseTargetPlanningPolicy(config));};
        if(strength=="require")CHECK_THROWS_WITH(compile(),Catch::Matchers::ContainsSubstring("target_planning.retain_unsatisfiable@1"));
        else {
            const auto result=compile();REQUIRE(result.plans.size()==1);
            CHECK(std::any_of(result.plans[0]->policy.begin(),result.plans[0]->policy.end(),[](const auto &e){
                return e.key=="retain" && e.status==PlanningPolicyStatus::infeasible_prefer;}));
        }
    }
}

TEST_CASE("RC-7c fix generation preserves packed prefer slot in both compilers", "[rendercompiler][rc7c-fix4]") {
    using Json=nlohmann::json;
    Json config{{"render_targets",Json::array()},{"rendering_passes",Json::array()}};
    for(const auto *name:{"A","B","C"})config["render_targets"].push_back({{"name",name},
        {"format","R8G8B8A8_UNORM"},{"format_class","data"},{"extent_scale",1.0},{"usage",{"COLOR_ATTACHMENT","SAMPLED"}}});
    const auto writer=[](const char *name,const char *resource) {return Json{{"name",name},{"type","fullscreen"},
        {"output",{{"color",resource},{"depth",nullptr}}},{"shader",{{"vertex","engine://fullscreen"},{"fragment","engine://passthrough"}}}};};
    const auto reader=[&](const char *name,const char *resource) {auto p=writer(name,"swapchain");p["input"]={resource};return p;};
    config["rendering_passes"].push_back({{"name","G1"},{"passes",Json::array({writer("wa","A"),reader("ra","A"),
        writer("wb","B"),reader("rb","B"),writer("wc","C"),reader("rc","C")})}});
    config["rendering_passes"].push_back({{"name","G2"},{"passes",Json::array({writer("wc","C"),writer("wa","A"),
        reader("ra","A"),writer("wb","B"),reader("rb","B"),reader("rc","C")})}});
    for(auto &g:config["rendering_passes"]) {
        std::string previous;
        for(auto &p:g["passes"]) {
            p["name"]=g["name"].get<std::string>()+"_"+p["name"].get<std::string>();
            if(!previous.empty())p["after"]={previous};previous=p["name"].get<std::string>();
        }
    }
    for(const auto *name:{"A","B","C"})config["target_planning"]["graphs"]["G1"]["resources"][name]["alias_group"]={{"value","same"},{"strength","prefer"}};
    const auto check=[](const VulkanTargetPlan &plan) {
        REQUIRE(plan.memory_packing.same_offset_sets==std::vector<std::vector<std::string>>{{"A","B"}});
        for(const auto &e:plan.policy)if(e.key=="alias_group") {
            CAPTURE(e.target);
            CHECK(e.status==(e.target=="C"?PlanningPolicyStatus::infeasible_prefer:PlanningPolicyStatus::adopted));
        }
        CHECK(std::any_of(plan.decisions.begin(),plan.decisions.end(),[](const auto &d){
            return d.id=="pelican.plan.alias_generation_contract@1" && d.subject=="C" && d.selected=="not_adopted";}));
    };
    SECTION("graph compiler") {
        const auto graphs=TestSupport::renderGraphOf(config);const auto targets=test_support::renderTargetDefinitionsOf(config);
        const auto result=compileRenderingTargetPlans(graphs,TestSupport::framePlanMapOf(graphs),targets,compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Srgb,TestSupport::rc7cDeviceFacts(),std::nullopt,std::nullopt,parseTargetPlanningPolicy(config));
        for(const auto &p:result.plans)if(p->graph=="G1")check(*p);
    }
    SECTION("program compiler") {
        // Each variant has one graph. XR's reordered C spans both A and B,
        // exercising the program-wide update after each graph compiled alone.
        auto single=config;single["rendering_passes"].erase(single["rendering_passes"].begin()+1);
        ProgramInputFixture fixture;fixture.useAuthoredConfig(single,"rc7c-prefer-slot");
        fixture.backend_context=VulkanRenderCompilerBackendContext{vk::Format::eB8G8R8A8Srgb,{64,64},{},{},{},VulkanRenderCompilerDevicePlanningMode::compiler_only};
        fixture.backend_context.planning_facts=TestSupport::rc7cDeviceFacts();
        fixture.variants={{.graph_variant=RenderPipelineGraphVariant::flat},
            {.graph_variant=RenderPipelineGraphVariant::xr,.compose_runtime_config=[](Json &composed) {
                auto &passes=composed["rendering_passes"][0]["passes"];
                const auto original=passes;passes=Json::array();
                std::string previous;
                for(const auto i:{4,0,1,2,3,5,6}) {
                    auto pass=original.at(i);pass.erase("after");
                    if(!previous.empty())pass["after"]={previous};
                    previous=pass["name"].get<std::string>();passes.push_back(std::move(pass));
                }
            }}};
        auto input=fixture.input();input.runtime_shader_compiler_enabled=true;
        const auto result=runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(),input);
        check(*requireVulkanRenderCompilerPhysicalPackage(*result.variants[0].physical_package).target_plans.at("G1"));
    }
}
