namespace {
using RecipeJson = nlohmann::json;
RecipeJson recipeRoot(std::string reference = "engine://render_pipelines/experimental/hybrid_recipe.json") {
    return {{"pipeline", {{"preset", std::move(reference)}}}};
}
RecipeJson recipeDocument() {
    return RecipeJson::parse(loadEngineFeature("engine://render_pipelines/experimental/hybrid_recipe.json"));
}
}

TEST_CASE("RC-5 A1 recipe expansion preserves every structure and policy field", "[render-feature][pipeline-preset][rc5][rc5-a1]") {
    const auto legacy = RecipeJson::parse(loadEngineFeature("engine://render_pipelines/hybrid_v1.json")).at("config");
    const auto recipe = recipeDocument();
    const auto expanded = expandRenderPipelineRecipe(recipe.at("config"), RecipeJson::object(), loadEngineFeature);
    REQUIRE(expanded.config.at("rendering_passes") == legacy.at("rendering_passes"));
    REQUIRE(expanded.config == legacy);
    REQUIRE_FALSE(expanded.config.contains("features"));
    const auto lit = RecipeJson::parse(loadEngineFeature("engine://render_pipelines/experimental/hybrid_recipe_lit.json"));
    auto expected = legacy;
    expected["features"] = {"engine://features/shadow_directional.json", "engine://features/sky_ambient.json"};
    REQUIRE(expandRenderPipelineRecipe(lit.at("config"), RecipeJson::object(), loadEngineFeature).config == expected);
    auto old_root = recipeRoot("engine://render_pipelines/hybrid_v1.json");
    old_root["features"] = expected.at("features");
    REQUIRE(resolveRenderPipelinePreset(recipeRoot("engine://render_pipelines/experimental/hybrid_recipe_lit.json"), loadEngineFeature).config ==
            resolveRenderPipelinePreset(old_root, loadEngineFeature).config);
    auto inherited_root = recipeRoot("engine://render_pipelines/experimental/hybrid_recipe_lit.json");
    inherited_root["features"] = {"engine://features/sky_ambient.json", "engine://features/ui.json"};
    expected["features"].push_back("engine://features/ui.json");
    const auto inherited = resolveRenderPipelinePreset(inherited_root, loadEngineFeature).config;
    REQUIRE(inherited.at("features") == expected.at("features"));
    // A project-owned @1 still inherits its original features with the same rule.
    auto v1 = RecipeJson{{"schema", "pelican.render_pipeline"}, {"version", 1}, {"name", "local"}, {"config", expected}};
    inherited_root["pipeline"]["preset"] = "test://v1";
    REQUIRE(resolveRenderPipelinePreset(inherited_root, [&](std::string_view) { return v1.dump(); }).config.at("features") == expected.at("features"));
}

TEST_CASE("RC-5 recipe keys and family shapes are explicit", "[render-feature][pipeline-preset][rc5]") {
    auto recipe = recipeDocument();
    const auto load = [&](std::string_view ref) {
        return ref == "test://recipe" ? recipe.dump() : loadEngineFeature(ref);
    };
    const auto resolve = [&] { return resolveRenderPipelinePreset(recipeRoot("test://recipe"), load); };
    for (const auto *key : {"render_targets", "rendering_passes", "snapshots", "buffers", "compute_tasks"}) {
        CAPTURE(key);
        auto saved = recipe;
        recipe["config"][key] = RecipeJson::array();
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.recipe_has_structure@1", {{"key", key}});
        recipe = saved;
    }
    for (const auto *key : {"families", "feature_overrides", "multisampling", "lighting_data", "pipeline", "material_route_default"}) {
        auto saved = recipe;
        recipe["config"][key] = RecipeJson::object();
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.recipe_unknown_key@1", {{"key", key}});
        recipe = saved;
    }
    for (const auto *key : {"features", "graph_transforms", "shader_defines", "draw_sort", "material_routing", "xr", "render_strategy", "target_planning", "vulkan_plan_pins", "vulkan_physical_fragments"}) {
        CAPTURE(key);
        auto saved = recipe;
        const auto value = (std::string{key} == "features" || std::string{key} == "graph_transforms" || std::string{key} == "shader_defines")
            ? RecipeJson::array() : RecipeJson::object();
        recipe["config"][key] = value;
        REQUIRE(resolve().config.at(key) == value);
        recipe = saved;
    }
    SECTION("base is required") {
        recipe["config"].erase("base");
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.recipe_invalid@1", {{"field", "base"}});
    }
    SECTION("base is nonempty") {
        recipe["config"]["base"] = RecipeJson::array();
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.recipe_invalid@1", {{"field", "base"}});
    }
    SECTION("families are unique") {
        recipe["config"]["base"].push_back(recipe.at("config").at("base").at(0));
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.recipe_invalid@1", {{"field", "family"}, {"reason", "duplicate_family"}});
    }
    SECTION("multiple empty families retain declaration order") {
        recipe["config"]["base"] = {{{"family", "first"}, {"fragments", RecipeJson::array()}}, {{"family", "second"}, {"fragments", RecipeJson::array()}}};
        REQUIRE(resolve().config.at("rendering_passes") == RecipeJson::array({{{"name", "first"}, {"passes", RecipeJson::array()}}, {{"name", "second"}, {"passes", RecipeJson::array()}}}));
    }
}

TEST_CASE("RC-5 recipe rejects duplicate pass and target names across families", "[render-feature][pipeline-preset][rc5]") {
    for (const auto *kind : {"pass", "target"}) {
        CAPTURE(kind);
        const std::string field = std::string{kind} == "pass" ? "passes" : "render_targets";
        const RecipeJson config{{"base", {
            {{"family", "F"}, {"fragments", {"test://a"}}},
            {{"family", "G"}, {"fragments", {"test://b"}}}}}};
        const auto load = [&](std::string_view ref) {
            RecipeJson fragment{{"schema", "pelican.render_fragment"}, {"version", 1},
                {"name", ref == "test://a" ? "a" : "b"},
                {"render_targets", RecipeJson::array()}, {"passes", RecipeJson::array()}};
            fragment[field].push_back({{"name", "p"}});
            return fragment.dump();
        };
        TestSupport::requireNamedRefusal(
            [&] { return expandRenderPipelineRecipe(config, RecipeJson::object(), load); },
            "render_pipeline.recipe_duplicate_name@1",
            {{"kind", kind}, {"name", "p"}, {"fragments", {"a", "b"}}});
    }
}

TEST_CASE("RC-5 A4 fragment overrides preserve position and reject invalid identities", "[render-feature][pipeline-preset][rc5][rc5-a4]") {
    auto root = recipeRoot();
    auto recipe = recipeDocument();
    auto replacement = RecipeJson::parse(loadEngineFeature("engine://render_fragments/deferred_lighting.json"));
    const auto load = [&](std::string_view ref) {
        if (ref == "project://lighting.json") return replacement.dump();
        if (ref == "engine://render_pipelines/experimental/hybrid_recipe.json") return recipe.dump();
        return loadEngineFeature(ref);
    };
    const auto resolve = [&] { return resolveRenderPipelinePreset(root, load); };
    SECTION("replacement occupies its inherited position") {
        const auto original = resolve();
        root["fragment_overrides"] = {{"deferred_lighting", "project://lighting.json"}};
        const auto replaced = resolve();
        REQUIRE(replaced.config.at("rendering_passes") == original.config.at("rendering_passes"));
        REQUIRE(replaced.config == original.config);
        REQUIRE(replaced.fragment_passes.at("deferred_lighting").reference == "project://lighting.json");
    }
    SECTION("unknown name reports inherited names") {
        root["fragment_overrides"] = {{"typo", nullptr}};
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.fragment_override_unknown@1",
            {{"name", "typo"}, {"inherited", {"deferred_geometry", "ssao", "deferred_lighting", "forward_base", "present"}}});
    }
    SECTION("v1 rejects overrides including empty objects") {
        root = recipeRoot("engine://render_pipelines/hybrid_v1.json");
        root["fragment_overrides"] = RecipeJson::object();
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.fragment_overrides_need_recipe@1");
    }
    SECTION("repeated names fail before overrides") {
        recipe["config"]["base"][0]["fragments"].push_back("engine://render_fragments/ssao.json");
        root["fragment_overrides"] = {{"ssao", nullptr}};
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.recipe_duplicate_fragment@1", {{"name", "ssao"}});
    }
    SECTION("replacement name must match") {
        replacement["name"] = "other";
        root["fragment_overrides"] = {{"deferred_lighting", "project://lighting.json"}};
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.fragment_override_name_mismatch@1", {{"name", "deferred_lighting"}, {"replacement_name", "other"}});
    }
    SECTION("removing SSAO is refused at expansion") {
        root["fragment_overrides"] = {{"ssao", nullptr}};
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.fragment_missing_target@1", {{"fragment", "deferred_lighting"}, {"target", "ssao_blur"}});
    }
    SECTION("only required_targets names an unavailable target") {
        replacement["required_targets"].push_back("only_declared_dependency");
        root["fragment_overrides"] = {{"deferred_lighting", "project://lighting.json"}};
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.fragment_missing_target@1", {{"fragment", "deferred_lighting"}, {"target", "only_declared_dependency"}});
    }
    SECTION("fragments have no insertion or parameter contract") {
        replacement["insert"] = "end";
        root["fragment_overrides"] = {{"deferred_lighting", "project://lighting.json"}};
        TestSupport::requireNamedRefusal(resolve, "render_pipeline.fragment_invalid@1", {{"field", "insert"}});
    }
}
