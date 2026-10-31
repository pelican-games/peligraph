TEST_CASE("RC-7c C6 missing and empty facts never share", "[target-render-planning][rc7c][rc7c-c6]") {
    Rc7bFixture f;
    for (const bool empty_query : {false,true}) {
        f.request.memory_requirements = empty_query
            ? decltype(f.request.memory_requirements){[](const auto &,auto)->std::optional<ImageMemoryRequirements>{return std::nullopt;}}
            : decltype(f.request.memory_requirements){};
        const auto plan=f.run();
        CHECK(plan.memory_regions.empty()); CHECK(plan.alias_groups.empty());
        REQUIRE_FALSE(plan.opportunities.alias_candidates.empty());
        const auto dump=vulkanTargetPlanToJson(plan);
        CHECK(dump.at("planning_opportunities").at("alias_candidates")[0].at("adoption")=="unknown");
        f.policy(rc6a2Group());
        CHECK_THROWS_WITH(f.run(),Catch::Matchers::ContainsSubstring("alias.requirements_unknown"));
        f.policy(nlohmann::json::object());
    }
}

TEST_CASE("RC-7c fix provenance replaces one variant of the same graph", "[target-render-planning][rc7c-fix3]") {
    Rc7bFixture v1, v2;
    v1.graph.name=v2.graph.name="G";
    v1.request.node_order=FramePlanNodeOrder{v1.graph,{"WriteA","ReadA","WriteB","ReadB"}};
    // V1 G={A,B}, V2 G={A,C}; both use the same graph name.
    v2.graph.resources[1].name="temporary_c";
    for(auto &n:v2.graph.nodes)for(auto &u:n.uses) {
        if(u.input_value && u.input_value->resource=="temporary_b")u.input_value->resource="temporary_c";
        if(u.output_value && u.output_value->resource=="temporary_b")u.output_value->resource="temporary_c";
    }
    v2.request.pattern_bindings[1].resource="temporary_c";
    v2.request.effective_image_usage["temporary_c"]=v2.request.effective_image_usage.at("temporary_b");
    v2.request.effective_image_usage.erase("temporary_b");
    for(auto &a:v2.request.automatic_attachments)if(a.logical_resource=="temporary_b")a.logical_resource="temporary_c";
    v2.request.node_order=FramePlanNodeOrder{v2.graph,{"WriteA","ReadA","WriteB","ReadB"}};
    auto a=std::make_shared<VulkanTargetPlan>(v1.run()),b=std::make_shared<VulkanTargetPlan>(v2.run());
    a->memory_provenance_id={"V1","G"};b->memory_provenance_id={"V2","G"};
    const std::vector<std::shared_ptr<const VulkanTargetPlan>> provenance{a,b};
    const auto memory=planGenerationMemory(provenance);
    auto published=*a;
    published.memory_provenance=provenance;published.memory_packing=memory.inputs;
    published.memory_requirements=memory.requirements;published.memory_regions=memory.regions;published.alias_groups=memory.alias_groups;
    published.automatic_plan_fingerprint=vulkanAutomaticTargetPlanFingerprint(topology(false),published);
    REQUIRE_NOTHROW(v1.verify(published,ejectVulkanCompletePhysicalPlanPackage(published)));
}

TEST_CASE("RC-7c fix wire reconstructs forbidden partial overlaps", "[target-render-planning][rc7c-fix5]") {
    Rc7bFixture f;
    addRead(f.types,f.graph.nodes.back(),f.graph.resources[0],1,"still_a",LogicalReadFootprintKind::same_pixel);
    const auto plan=f.run();
    nlohmann::json wire=vulkanTargetPlanToJson(plan);
    wire["alias_groups"]=nlohmann::json::array();
    wire["memory_packing"]["forbidden_pairs"]=nlohmann::json::array();
    wire["memory_regions"]=nlohmann::json::array({{{"region",0},{"memory_type_bits",1},{"size",768},{"alignment",256},
        {"residents",nlohmann::json::array({{{"resource","temporary_a"},{"offset",0},{"size",512},{"alignment",256}},
                                         {{"resource","temporary_b"},{"offset",256},{"size",512},{"alignment",256}}})}}});
    const auto result=validatePhysicalTargetPlanWire(&wire);
    CHECK_FALSE(result.available());
    CHECK(result.reason().find("memory.overlap@1")!=std::string::npos);
    // Disjoint lifetimes inside one fused scope also forbid partial overlap.
    wire["resources"][0]["lifetime"]["last_use"]=1;
    wire["scopes"][0]["nodes"]={"WriteA","ReadA","WriteB","ReadB"};
    wire["scopes"].erase(wire["scopes"].begin()+1,wire["scopes"].end());
    const auto scoped=validatePhysicalTargetPlanWire(&wire);
    CHECK_FALSE(scoped.available());
    CHECK(scoped.reason().find("memory.overlap@1")!=std::string::npos);
}

TEST_CASE("RC-7c C6 different images share a key and same offset with different sizes", "[target-render-planning][rc7c][rc7c-c6]") {
    Rc7bFixture f;
    f.request.pattern_bindings[1].extent=ResourceExtentPlan{.kind=ResourceExtentKind::fixed,.width=32,.height=32};
    f.request.pattern_bindings[1].pattern.format_candidates={{.format=ImageFormat::r16g16b16a16_sfloat}};
    f.policy(rc6a2Group());
    const auto plan=f.run();
    REQUIRE(plan.memory_regions.size()==1);
    const auto &r=plan.memory_regions.front();
    REQUIRE(r.residents.size()==2);
    CHECK(r.residents[0].offset==r.residents[1].offset);
    CHECK(r.residents[0].size!=r.residents[1].size);
    REQUIRE(plan.alias_groups.size()==1);
    CHECK(plan.alias_groups.front().resources==std::vector<std::string>{"temporary_a","temporary_b"});
    REQUIRE_NOTHROW(f.verify(plan,ejectVulkanCompletePhysicalPlanPackage(plan)));
}

TEST_CASE("RC-7c C6 memory keys and dedicated facts control region admission", "[target-render-planning][rc7c][rc7c-c6]") {
    for (const bool dedicated : {false,true}) {
        Rc7bFixture f;
        f.request.memory_requirements=[dedicated](const auto &r,auto u) {
            auto req=TestSupport::rc7cMemoryRequirements(r,u);
            if(r.logical_resource=="temporary_b") {req->requires_dedicated=dedicated; if(!dedicated)req->memory_type_bits=2;}
            return req;
        };
        const auto plan=f.run();
        CHECK(plan.alias_groups.empty());
        CHECK(plan.memory_regions.size()==(dedicated?1:2));
        f.policy(rc6a2Group());
        CHECK_THROWS_AS(f.run(),GraphRegistryError);
        auto package=ejectVulkanCompletePhysicalPlanPackage(plan);
        if (dedicated) package.memory_regions[0].residents.push_back({"temporary_b",0,16384,256});
        else package.memory_regions[0].memory_type_bits=3;
        CHECK_THROWS_WITH(f.verify(plan,package),Catch::Matchers::ContainsSubstring(dedicated?"memory.dedicated@1":"memory.key@1"));
    }
}

TEST_CASE("RC-7c required same offset slot wins over intervening live image", "[target-render-planning][rc7c][rc7c-slot]") {
    Rc7bFixture f;
    f.graph.resources.push_back({.name="middle",.type=f.graph.resources[0].type});
    LogicalGraphNode a{.name="WriteA"},x{.name="WriteX"},b{.name="WriteB"},read{.name="ReadX"};
    addWrite(f.types,a,f.graph.resources[0],1,"out");
    addWrite(f.types,x,f.graph.resources[2],1,"out");
    addWrite(f.types,b,f.graph.resources[1],1,"out");
    addRead(f.types,read,f.graph.resources[2],1,"in",LogicalReadFootprintKind::same_pixel);
    f.graph.nodes={a,x,b,read};
    f.request.node_order=FramePlanNodeOrder{f.graph,{"WriteA","WriteX","WriteB","ReadX"}};
    auto binding=f.request.pattern_bindings[0];binding.resource="middle";f.request.pattern_bindings.push_back(binding);
    f.request.effective_image_usage["middle"]=f.request.effective_image_usage.at("temporary_a");
    f.request.automatic_attachments={{.node="WriteA",.logical_resource="temporary_a",.operations=PassAttachmentOperations{AttachmentLoadOp::clear,AttachmentStoreOp::store}},
        {.node="WriteX",.logical_resource="middle",.operations=PassAttachmentOperations{AttachmentLoadOp::clear,AttachmentStoreOp::store}},
        {.node="WriteB",.logical_resource="temporary_b",.operations=PassAttachmentOperations{AttachmentLoadOp::clear,AttachmentStoreOp::store}}};
    f.policy(rc6a2Group());
    const auto plan=f.run();
    REQUIRE(plan.memory_regions.size()==1);
    std::map<std::string,std::uint64_t> offsets;
    for(const auto &r:plan.memory_regions[0].residents)offsets[r.resource]=r.offset;
    CHECK(offsets.at("temporary_a")==offsets.at("temporary_b"));
    CHECK(offsets.at("middle")!=offsets.at("temporary_b"));
    CHECK(plan.memory_packing.same_offset_sets==std::vector<std::vector<std::string>>{{"temporary_a","temporary_b"}});
}

TEST_CASE("RC-7c C6 generation intersects resource eligibility and pair lifetimes", "[target-render-planning][rc7c][rc7c-c6][rc7c-agreement]") {
    Rc7bFixture f;
    const auto legal=std::make_shared<const VulkanTargetPlan>(f.run());
    addRead(f.types,f.graph.nodes.back(),f.graph.resources[0],1,"still_a",LogicalReadFootprintKind::same_pixel);
    f.graph.name="overlap";
    f.request.node_order=FramePlanNodeOrder{f.graph,{"WriteA","ReadA","WriteB","ReadB"}};
    const auto overlapping=std::make_shared<const VulkanTargetPlan>(f.run());
    const std::vector plans{legal,overlapping};
    const auto memory=planGenerationMemory(plans);
    REQUIRE(memory.regions.size()==1);
    REQUIRE(memory.regions[0].residents.size()==2);
    CHECK(memory.regions[0].residents[0].offset!=memory.regions[0].residents[1].offset);
    CHECK(memory.alias_groups.empty());
    REQUIRE(memory.inputs.forbidden_pairs.size()==1);
    Rc7bFixture required; required.policy(rc6a2Group());
    const std::vector required_plans{std::make_shared<const VulkanTargetPlan>(required.run()),overlapping};
    CHECK_THROWS_WITH(planGenerationMemory(required_plans),Catch::Matchers::ContainsSubstring("target_planning.alias_generation_contract@1"));
    // One graph where the second resource starts with load excludes it everywhere.
    Rc7bFixture load; load.graph.name="load";load.request.node_order=FramePlanNodeOrder{load.graph,{"WriteA","ReadA","WriteB","ReadB"}};load.request.automatic_attachments[1].operations.load_op=AttachmentLoadOp::load;
    load.graph.nodes[2].uses[0]=makeLogicalReadWriteUse("out",{"temporary_b",0},{"temporary_b",1},{LogicalReadFootprintKind::same_pixel,std::nullopt},LogicalAccessIntent::attachment);
    load.graph.nodes[2].ports[0].direction=LogicalPortDirection::input_output;
    load.graph.imports.push_back({.value={"temporary_b",0}});
    const std::vector load_plans{legal,std::make_shared<const VulkanTargetPlan>(load.run())};
    const auto excluded=planGenerationMemory(load_plans);
    CHECK(excluded.exclusions.contains("temporary_b"));
    for(const auto &region:excluded.regions)for(const auto &r:region.residents)CHECK(r.resource!="temporary_b");
    // Resources which never occur together cannot overlap.
    auto only_a=std::make_shared<VulkanTargetPlan>(*legal),only_b=std::make_shared<VulkanTargetPlan>(*legal);
    only_a->resources.resize(1); only_a->memory_requirements.erase("temporary_b");
    only_b->resources.erase(only_b->resources.begin());only_b->memory_requirements.erase("temporary_a");
    const std::vector<std::shared_ptr<const VulkanTargetPlan>> separate{only_a,only_b};
    const auto apart=planGenerationMemory(separate);
    CHECK(apart.alias_groups.empty()); CHECK(apart.inputs.forbidden_pairs.size()==1);
}

TEST_CASE("RC-7c C6 verifier rejects alignment heap key and maximal same offset independently", "[target-render-planning][rc7c][rc7c-c6][rc7c-verifier]") {
    Rc7bFixture f;f.policy(rc6a2Group());
    const auto plan=f.run();
    const std::string mutation=GENERATE("alignment","heap","key","key_input","set_input","same_offset","groups","inputs","legacy");
    auto package=ejectVulkanCompletePhysicalPlanPackage(plan);
    std::string code;
    if(mutation=="alignment"){package.memory_regions[0].residents[0].offset=1;code="memory.alignment@1";}
    if(mutation=="heap"){++package.memory_regions[0].size;code="memory.heap_size@1";}
    if(mutation=="key"){package.memory_regions[0].memory_type_bits=3;code="memory.key@1";}
    if(mutation=="key_input"){package.memory_packing.resources[0].memory_type_bits=3;code="memory.key@1";}
    if(mutation=="set_input"){package.memory_packing.same_offset_sets.clear();code="memory.same_offset@1";}
    if(mutation=="same_offset"){auto &r=package.memory_regions[0];r.residents[1].offset=r.residents[0].size;r.size=r.residents[1].offset+r.residents[1].size;code="memory.same_offset@1";}
    if(mutation=="groups"){package.alias_groups.clear();code="memory.same_offset@1";}
    if(mutation=="inputs"){package.memory_packing.order.clear();code="memory.inputs@1";}
    if(mutation=="legacy"){
        auto json=vulkanCompletePhysicalPlanPackageToJson(package);json.erase("memory_regions");
        CHECK_THROWS_WITH(vulkanCompletePhysicalPlanPackageFromJson(json),Catch::Matchers::ContainsSubstring("complete_physical_plan.memory_regions_required@1"));
    } else CHECK_THROWS_WITH(f.verify(plan,package),Catch::Matchers::ContainsSubstring(code));
}

TEST_CASE("RC-7c C6 verifier recomputes forbidden overlaps from provenance", "[target-render-planning][rc7c][rc7c-c6][rc7c-overlap]") {
    Rc7bFixture f;
    addRead(f.types,f.graph.nodes.back(),f.graph.resources[0],1,"still_a",LogicalReadFootprintKind::same_pixel);
    const auto plan=f.run();
    auto package=ejectVulkanCompletePhysicalPlanPackage(plan);
    REQUIRE(package.memory_regions.size()==1);REQUIRE(package.memory_regions[0].residents.size()==2);
    REQUIRE(package.memory_regions[0].residents[1].offset!=0);
    package.memory_regions[0].residents[1].offset=0;
    package.memory_regions[0].size=std::max(package.memory_regions[0].residents[0].size,package.memory_regions[0].residents[1].size);
    package.memory_packing.forbidden_pairs.clear();
    CHECK_THROWS_WITH(f.verify(plan,package),Catch::Matchers::ContainsSubstring("memory.overlap@1"));
}

TEST_CASE("RC-7c C6 dropped retain prefer leaves no transfer usage and device refusal remains", "[target-render-planning][rc7c][rc7c-c6]") {
    Rc7bFixture f;
    for(auto &[name,u]:f.request.effective_image_usage)u=ImageUsageBit::color_attachment|ImageUsageBit::sampled;
    auto entries=rc6a2Group();entries["resources"]["temporary_b"]["retain"]={{"value",true},{"strength","prefer"}};
    f.policy(entries);
    const auto plan=f.run();
    CHECK_FALSE(bool(plan.effective_image_usage.at("temporary_b") & ImageUsageBit::transfer_src));
    CHECK(physicalResource(plan,"temporary_b").lifetime.retain.empty());
    REQUIRE(plan.alias_groups.size()==1);
    CHECK(std::any_of(plan.policy.begin(),plan.policy.end(),[](const auto &e){return e.key=="retain"&&e.status==PlanningPolicyStatus::infeasible_prefer;}));
    f.policy({{"resources",{{"temporary_b",{{"retain",true}}}}}});
    f.request.supports_image_usage=[](const auto &,ImageUsage u){return !bool(u&ImageUsageBit::transfer_src);};
    CHECK_THROWS_WITH(f.run(),Catch::Matchers::ContainsSubstring("target_planning.retain_unsatisfiable@1"));
    f.policy({{"resources",{{"temporary_b",{{"retain",{{"value",true},{"strength","prefer"}}}}}}}});
    const auto dropped=f.run();
    CHECK_FALSE(bool(dropped.effective_image_usage.at("temporary_b") & ImageUsageBit::transfer_src));
}
