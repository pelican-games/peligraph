namespace Pelican {
TEST_CASE("RC-7a F3 RPC retain true false null and exact Undo preserve other policy", "[render-config-editor][rc7a][rc7-f3]") {
    Rc5bHarness h;
    const auto before = readBytes(h.project.render_config);
    const auto edit = [&](Json value) {
        return h.commit(h.service->editTargetPlanning({{"base_source_digest", h.context().at("source_digest").at("hex")},
            {"graph", "main_render"}, {"resources", {{"lit_color", {{"retain", value}}}}}}));
    };
    edit(true);
    const auto with_true = readBytes(h.project.render_config);
    CHECK(Json::parse(with_true)["rendering"]["target_planning"]["graphs"]["main_render"]["resources"]["lit_color"]["retain"] == true);
    h.commit(h.service->editTargetPlanning({{"base_source_digest", h.context().at("source_digest").at("hex")},
        {"graph", "main_render"}, {"resources", {{"lit_color", {{"no_alias", true}}}}}}));
    CHECK(Json::parse(readBytes(h.project.render_config))["rendering"]["target_planning"]["graphs"]["main_render"]["resources"]["lit_color"]["retain"] == true);
    edit(false);
    const auto with_false = readBytes(h.project.render_config);
    CHECK(Json::parse(with_false)["rendering"]["target_planning"]["graphs"]["main_render"]["resources"]["lit_color"]["retain"] == false);
    edit(nullptr);
    const auto erased = readBytes(h.project.render_config);
    CHECK(erased.find("\"retain\"") == std::string::npos);
    h.commit(h.service->restoreRenderDocuments({{"documents", Json::array({rd1Document("passes/main.json", renderConfigSourceDigest(erased), with_true)})}}));
    CHECK(readBytes(h.project.render_config) == with_true);
    h.commit(h.service->restoreRenderDocuments({{"documents", Json::array({rd1Document("passes/main.json", renderConfigSourceDigest(with_true), before)})}}));
    CHECK(readBytes(h.project.render_config) == before);
}
}
