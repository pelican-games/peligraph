TEST_CASE("RC-7d D5 verifier checks kind before membership and materialized store", "[rc7d][rc7d-d5]") {
    Rc7bFixture f;
    const auto plan=f.run();
    auto package=ejectVulkanCompletePhysicalPlanPackage(plan);
    auto lazy=plan;
    lazy.effective_image_usage["temporary_a"]=ImageUsageBit::color_attachment|ImageUsageBit::transient_attachment;
    lazy.memory_types={{0x11u,0}};
    // Deliberately break other claims: lazy preflight must win.
    package.resources[0].aliasable=false;package.resources[0].stored=false;
    CHECK_THROWS_WITH(f.verify(lazy,package),Catch::Matchers::ContainsSubstring("memory.lazy_in_region@1: temporary_a"));
    nlohmann::json wire=vulkanTargetPlanToJson(lazy);
    const auto refusal=validatePhysicalTargetPlanWire(&wire);
    CHECK_FALSE(refusal.available());CHECK(refusal.reason().find("memory.lazy_in_region@1")!=std::string::npos);
    package=ejectVulkanCompletePhysicalPlanPackage(plan);package.resources[0].stored=false;
    CHECK_THROWS_WITH(f.verify(plan,package),Catch::Matchers::ContainsSubstring("memory.stored@1"));
    wire=vulkanTargetPlanToJson(plan);wire["resources"][0]["stored"]=false;
    CHECK_FALSE(validatePhysicalTargetPlanWire(&wire).available());
}
TEST_CASE("RC-7d D5 wire takes a transient resident's kind from its representation", "[rc7d][rc7d-d5]") {
    Rc7bFixture f;
    const auto plan=f.run();
    const auto wireWith=[&](const char *representation,std::uint32_t flags) {
        nlohmann::json wire=vulkanTargetPlanToJson(plan);
        for(auto &r:wire["resources"])if(r["logical_resource"]=="temporary_a")r["representation"]=representation;
        wire["memory_types"]=nlohmann::json::array({{{"property_flags",flags},{"heap_index",0}}});
        wire["image_memory_facts"]["temporary_a"]={{"transient",false},{"memory_type_bits",1}};
        return wire;
    };
    for(const auto *rep:{"transient_attachment","tile_local_attachment"}) {
        CAPTURE(rep);
        auto lazy=wireWith(rep,0x11u);
        const auto refusal=validatePhysicalTargetPlanWire(&lazy);
        CHECK_FALSE(refusal.available());
        CHECK(refusal.reason().find("memory.lazy_in_region@1: temporary_a")!=std::string::npos);
        auto ordinary=wireWith(rep,0x1u);
        CHECK(validatePhysicalTargetPlanWire(&ordinary).available());
    }
    auto materialized=wireWith("materialized_image",0x11u);
    CHECK(validatePhysicalTargetPlanWire(&materialized).available());
}
TEST_CASE("RC-7d D5 Freeze keeps transient alias memberships preferred", "[rc7d][rc7d-d5][freeze]") {
    Rc7bFixture f;auto plan=f.run();
    auto wire=vulkanTargetPlanToJson(plan);
    for(auto &r:wire["resources"])if(r["logical_resource"]=="temporary_a")r["representation"]="transient_attachment";
    const auto frozen=PelicanStudio::freezeFramePlanPolicy(wire,{"WriteA","ReadA","WriteB","ReadB"},{"WriteA","ReadA","WriteB","ReadB"},{},nlohmann::json::object());
    CHECK(frozen.at("resources").at("temporary_a").at("alias_group").at("strength")=="prefer");
    CHECK(frozen.at("resources").at("temporary_b").at("alias_group").at("strength")=="require");
    for(auto &r:wire["resources"])if(r["logical_resource"]=="temporary_a")r["representation"]="tile_local_attachment";
    const auto tile=PelicanStudio::freezeFramePlanPolicy(wire,{},{},{},nlohmann::json::object());
    CHECK(tile.at("resources").at("temporary_a").at("alias_group").at("strength")=="prefer");
}
