TEST_CASE("RC-6a2 compiler binds new root recipe and part keys through the production bridge", "[rendercompiler][rc6a2]") {
    auto root = rc5Document(true, true, false);
    root["rendering"]["pipeline"]["preset"] = "project://recipe.json";
    root["rendering"]["features"].push_back("project://features/local.json");
    auto feature = Rc6Json::parse(PathResolver{}.loadText("engine://features/bloom.json"));
    auto recipe = Rc6Json::parse(PathResolver{}.loadText("engine://render_pipelines/experimental/hybrid_recipe.json"));
    const auto compile = [&] { return rc6Physical(rc5CompileWithFacts(root, false, &feature, {{"recipe.json", recipe}})); };
    const auto automatic = compile();
    REQUIRE(rc6InRegion(automatic, "Bloom_Threshold_RT"));
    REQUIRE_FALSE(automatic.contains("policy"));
    recipe["config"]["target_planning"] = {{"graphs", {{"main_render", {{"backend", {{"value", "transient"}, {"strength", "prefer"}}}}}}}};
    feature["target_planning"] = rc6Policy("resources", "Bloom_Threshold_RT", "representation", "materialized");
    root["rendering"]["target_planning"] = rc6Policy("nodes", "forward_opaque", "scope_break", true);
    root["rendering"]["target_planning"]["graphs"]["main_render"]["backend"] = "materialized";
    const auto result = compile();
    CHECK(result.at("backend_selection").at("selected_candidate") == "pelican.vulkan.materialized_plan@1");
    CHECK(rc6Row(result, "", "root").at("satisfied") == true);
    CHECK(rc6Row(result, "", "recipe").at("status") == "overridden");
    CHECK(rc6Row(result, "Bloom_Threshold_RT", "part").at("key") == "representation");
    CHECK(rc6Row(result, "Bloom_Threshold_RT", "part").at("origin").at("document") == "project://features/local.json");
    CHECK(rc6Row(result, "forward_opaque", "root").at("satisfied") == true);
}

TEST_CASE("RC-6a2 engine isolated sprite cannot satisfy a scope join", "[rendercompiler][rc6a2][rc6-c3]") {
    auto root = rc5Document(false, false, false);
    root["rendering"]["features"] = {"engine://features/sprite.json"};
    const auto automatic = rc6Physical(rc5CompileWithFacts(root, false));
    REQUIRE(std::any_of(automatic.at("scopes").begin(), automatic.at("scopes").end(), [](const auto &s) {
        return s.at("nodes") == Rc6Json::array({"sprite_draw"});
    }));
    root["rendering"]["target_planning"] = rc6Policy("nodes", "sprite_draw", "scope_join", true);
    CHECK(rc6Refusal([&] { rc5CompileWithFacts(root, false); }) == "target_planning.requirement_unsatisfied@1");
    root["rendering"]["target_planning"] = rc6Policy("nodes", "sprite_draw", "scope_join", {{"value", true}, {"strength", "prefer"}});
    const auto plan = rc6Physical(rc5CompileWithFacts(root, false));
    CHECK(rc6Row(plan, "sprite_draw", "root").at("status") == "infeasible_prefer");
}
