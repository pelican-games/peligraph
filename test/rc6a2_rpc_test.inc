namespace Pelican {
TEST_CASE("RC-6a2 RPC saves all five policy keys and erases backend", "[render-config-editor][rc6a2][rc6-rpc]") {
    Rc5bHarness h;
    const Json backend{{"value", "materialized"}, {"strength", "prefer"}};
    const auto before = readBytes(h.project.render_config);
    Json request{{"base_source_digest", h.context().at("source_digest").at("hex")}, {"graph", "main_render"},
        {"backend", backend},
        {"nodes", {{"forward_opaque", {{"scope_break", {{"value", true}, {"strength", "require"}}}, {"scope_join", false}}}}},
        {"resources", {{"g_emissive", {{"representation", "materialized"}, {"alias_group", "scratch"}}}}}};
    h.commit(h.service->editTargetPlanning(request));
    const auto after = readBytes(h.project.render_config);
    const auto saved = Json::parse(after).at("rendering").at("target_planning").at("graphs").at("main_render");
    CHECK(saved.at("backend") == backend);
    CHECK(saved.at("nodes") == request.at("nodes"));
    CHECK(saved.at("resources") == request.at("resources"));
    h.commit(h.service->restoreRenderDocuments({{"documents", Json::array({rd1Document("passes/main.json", renderConfigSourceDigest(after), before)})}}));
    CHECK(readBytes(h.project.render_config) == before);
    h.commit(h.service->restoreRenderDocuments({{"documents", Json::array({rd1Document("passes/main.json", renderConfigSourceDigest(before), after)})}}));
    CHECK(readBytes(h.project.render_config) == after);
    h.commit(h.service->editTargetPlanning({{"base_source_digest", h.context().at("source_digest").at("hex")},
        {"graph", "main_render"}, {"backend", nullptr}}));
    const auto erased = Json::parse(readBytes(h.project.render_config)).at("rendering").at("target_planning").at("graphs").at("main_render");
    CHECK_FALSE(erased.contains("backend"));
    CHECK(erased.at("resources") == saved.at("resources"));
}
}
