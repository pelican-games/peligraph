namespace {
Rc6a2Json rc6b1Shape(const VulkanTargetPlan &plan) {
    Rc6a2Json result{{"backend", plan.backend_selection.selected_candidate}, {"scopes", Rc6a2Json::array()}, {"aliases", Rc6a2Json::array()}, {"representations", Rc6a2Json::object()}};
    for (const auto &scope : plan.scopes) result["scopes"].push_back(scope.nodes);
    for (const auto &group : plan.alias_groups) { auto members = group.resources; std::sort(members.begin(), members.end()); result["aliases"].push_back(members); }
    for (const auto &resource : plan.resources) result["representations"][resource.logical_resource] = vulkanResourceRepresentationName(resource.representation);
    return result;
}
Rc6a2Json rc6b1Freeze(const CompiledLogicalRenderGraph &graph, const VulkanTargetPlan &plan, Rc6a2Json root = Rc6a2Json::object()) {
    std::set<std::string> names;
    for (const auto &node : graph.nodes) names.insert(node.name);
    return PelicanStudio::freezeFramePlanPolicy(vulkanTargetPlanToJson(plan), fixtureOrder(graph).nodes(), names, {}, root);
}
}
TEST_CASE("RC-6b1 D2 Freeze round trip preserves decisions and scope constraints survive changed view selection", "[target-render-planning][rc6b1][rc6-d2-effect]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    auto graph = hybridGraph(types, 3, false);
    for (auto &node : graph.nodes) if (node.name == "Forward") node.view_select = {{"face", 1}};
    const auto automatic = rc6a2Compile(types, graph);
    REQUIRE(automatic.backend_selection.selected_candidate == "pelican.vulkan.tile_local_plan@1");
    REQUIRE_FALSE(oneScopeContains(automatic, "Lighting", "Forward"));
    const auto frozen = rc6b1Freeze(graph, automatic);
    CHECK(rc6b1Shape(rc6a2Compile(types, graph, frozen)) == rc6b1Shape(automatic));
    // Legal input edit: Forward now draws every view, so automatic fusion changes.
    for (auto &node : graph.nodes) if (node.name == "Forward") node.view_select.clear();
    const auto changed = rc6a2Compile(types, graph);
    REQUIRE(oneScopeContains(changed, "Lighting", "Forward"));
    REQUIRE(rc6b1Shape(changed).at("scopes") != rc6b1Shape(automatic).at("scopes"));
    const auto kept = rc6a2Compile(types, graph, frozen);
    CHECK_FALSE(oneScopeContains(kept, "Lighting", "Forward"));
    CHECK(rc6b1Shape(kept).at("scopes") == rc6b1Shape(automatic).at("scopes"));
}
TEST_CASE("RC-6b1 D2 alias representation round trip uses collision-free root groups and opposite keys", "[target-render-planning][rc6b1][rc6-d2-alias]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto graph = rc6a2AliasGraph(types);
    const auto automatic = rc6a2Compile(types, graph, Rc6a2Json::object(), false);
    REQUIRE(automatic.alias_groups.size() == 1);
    Rc6a2Json root{{"resources", {{"unrelated", {{"alias_group", {{"value", "frozen_1"}, {"strength", "prefer"}}}}}}}};
    const auto frozen = rc6b1Freeze(graph, automatic, root);
    CHECK(frozen.at("resources").at("temporary_a").at("alias_group").at("value") == "frozen_2");
    CHECK(frozen.at("resources").at("temporary_a").at("representation").at("value") == "materialized");
    CHECK(rc6b1Shape(rc6a2Compile(types, graph, frozen, false)) == rc6b1Shape(automatic));
    for (const auto &node : frozen.at("nodes")) {
        CHECK(node.contains("scope_break")); CHECK(node.contains("scope_join"));
        CHECK(node.at("scope_break").at("value") != node.at("scope_join").at("value"));
    }
}
