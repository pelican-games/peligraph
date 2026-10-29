namespace Pelican {
TEST_CASE("RC-6b1 fix authoring context remains readable when strict IR refuses a pass", "[render-config-editor][rc6b1][rc6-fix-context]") {
    Rd1Harness h{true};
    h.service = makeProjectService(h.project, h.resolver, h.runtime, {}, {}, {}, {},
        [](RenderConfigEditorDependencies &dependencies) {
            const auto resolve = dependencies.resolve_authoring_context;
            dependencies.resolve_authoring_context = [resolve](const RenderConfigCandidateDocumentSet &documents) {
                auto result = resolve(documents);
                for (auto &graph : result.config["rendering_passes"]) for (auto &pass : graph["passes"])
                    if (pass.value("type", std::string{}) == "fullscreen" && !pass["output"]["color"].empty()) {
                        pass["input"] = {pass["output"]["color"][0]["target"]};
                        return result;
                    }
                FAIL("fixture needs a fullscreen pass");
                return result;
            };
        });
    const auto context = h.context();
    CHECK(context.at("source_digest").at("hex") == renderConfigSourceDigest(readBytes(h.project.render_config)));
    CHECK_FALSE(context.at("graphs").empty());
    CHECK(context.at("planning_catalog").empty());
    CHECK_FALSE(context.at("planning_catalog_error").get<std::string>().empty());
}

TEST_CASE("RC-6b1 fix Blank Freeze filters runtime resources before production RPC", "[render-config-editor][rc6b1][rc6-fix-blank]") {
    Rd1Harness h{true};
    auto root = Json::parse(readBytes(h.project.render_config));
    root["nodes"] = Json::array();
    root["rendering"] = {{"rendering_passes", Json::array({{{"name", "main_render"}, {"passes", Json::array({
        {{"name", "present"}, {"type", "fullscreen"}, {"input", Json::array()},
         {"output", {{"color", Json::array({{{"target", "display"}}})}, {"depth", nullptr}}},
         {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}}
    })}}})}};
    writeBytes(h.project.render_config, root.dump(2));
    h.service = makeProjectService(h.project, h.resolver, h.runtime);
    h.commit(h.service->editTargetPlanning({{"base_source_digest", h.context().at("source_digest").at("hex")},
        {"graph", "main_render"}, {"backend", PelicanStudio::planningRequire("materialized")}}));
    root = Json::parse(readBytes(h.project.render_config));
    root["rendering"]["features"] = {"engine://features/picking.json"};
    const GraphTransformRegistrySnapshot transforms;
    const RenderStrategyRegistrySnapshot strategies;
    const SubgraphReplacementRegistrySnapshot replacements;
    const VulkanRenderCompilerBackendContext backend{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{1280, 720}, {}, {}, {}, VulkanRenderCompilerDevicePlanningMode::compiler_only};
    const std::array variants{RenderCompilerProgramVariantRequest{.graph_variant = RenderPipelineGraphVariant::flat,
        .compose_runtime_config = [](Json &config) { appendImGuiPassToCanonicalGraphs(config); }}};
    const auto compiled = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), RenderCompilerProgramInput{
        .rendering_config = root, .source_name = "rc6-fix-blank", .path_resolver = h.resolver,
        .load_document = loadEngineDocument, .runtime_shader_compiler_enabled = true,
        .graph_transforms = transforms, .render_strategies = strategies, .subgraph_replacements = replacements,
        .backend_context = backend, .variants = variants});
    const auto &variant = compiled.variants.at(0);
    const auto physical = vulkanTargetPlanToJson(*requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package).target_plans.at("main_render"));
    const auto context = h.context();
    const auto &catalog = context.at("planning_catalog").at("main_render");
    std::set<std::string> nodes = catalog.at("nodes").get<std::set<std::string>>();
    std::set<std::string> resources = catalog.at("resources").get<std::set<std::string>>();
    REQUIRE_FALSE(resources.contains("picking_id"));
    REQUIRE(std::any_of(physical.at("resources").begin(), physical.at("resources").end(), [](const auto &resource) { return resource.at("logical_resource") == "picking_id"; }));
    const auto policy = PelicanStudio::freezeFramePlanPolicy(physical,
        [&] { std::vector<std::string> order; for (const auto &node : variant.frame_plans.at("main_render").nodes) order.push_back(node.name); return order; }(), nodes, {}, Json::object(), {}, &resources);
    auto raw = PelicanStudio::freezeFramePlanPolicy(physical,
        [&] { std::vector<std::string> order; for (const auto &node : variant.frame_plans.at("main_render").nodes) order.push_back(node.name); return order; }(), nodes, {}, Json::object());
    raw["graph"] = "main_render";
    raw["base_source_digest"] = context.at("source_digest").at("hex");
    const auto refusal = rd1Error(h.service->editTargetPlanning(raw));
    CHECK(refusal.at("name").get<std::string>().starts_with("picking_"));
    CHECK_FALSE(policy.at("resources").contains("picking_id"));
    auto request = policy;
    request["graph"] = "main_render";
    request["base_source_digest"] = context.at("source_digest").at("hex");
    h.commit(h.service->editTargetPlanning(request));
    vf1CompileAsRenderer(RenderConfigCandidateDocumentSet{"test://root", {{.reference = "test://root",
        .normalized_reference = "test://root", .path = h.project.render_config,
        .operation = RenderConfigDocumentOperation::replace,
        .expected = {.existence = RenderConfigDocumentExistence::present, .digest = renderConfigSourceDigest(readBytes(h.project.render_config))},
        .bytes = readBytes(h.project.render_config)}}}, h.resolver);
}

TEST_CASE("RC-6b1 D3 absent saved leaves erase atomically and exact handwritten bytes undo redo", "[render-config-editor][rc6b1][rc6-d3-rpc]") {
    Rc5bHarness h;
    auto root = Json::parse(readBytes(h.project.render_config));
    auto &graph = root["rendering"]["target_planning"]["graphs"]["main_render"];
    graph = {{"backend", {{"value", "materialized"}, {"strength", "prefer"}}},
        {"nodes", {{"gone", {{"scope_break", true}, {"scope_join", {{"value", false}, {"strength", "require"}}}, {"serial", false}, {"isolate", false}, {"fuse", true}}}}},
        {"resources", {{"gone_resource", {{"alias_group", "handwritten"}, {"representation", {{"value", "materialized"}}}, {"no_alias", true}}}}}};
    const auto original = graph;
    writeBytes(h.project.render_config, root.dump(3) + "\n");
    h.service = makeProjectService(h.project, h.resolver, h.runtime);
    const auto before = readBytes(h.project.render_config);
    const auto digest = h.context().at("source_digest").at("hex");
    Json request{{"base_source_digest", digest}, {"graph", "main_render"}, {"backend", nullptr},
        {"nodes", {{"gone", {{"scope_break", nullptr}, {"scope_join", nullptr}}}}},
        {"resources", {{"gone_resource", {{"alias_group", nullptr}, {"representation", nullptr}}}}}};
    SECTION("unknown missing leaves and mixed writes are still refused") {
        auto invalid = request;
        invalid["nodes"]["gone"]["scope_join"] = false;
        CHECK(rd1Error(h.service->editTargetPlanning(invalid)).at("code") == "target_planning_name_unknown");
        invalid = request; invalid["nodes"]["gone"]["not_saved"] = nullptr;
        CHECK(rd1Error(h.service->editTargetPlanning(invalid)).at("code") == "target_planning_name_unknown");
        CHECK(readBytes(h.project.render_config) == before);
    }
    SECTION("Remove and exact document restoration") {
        h.commit(h.service->editTargetPlanning(request));
        const auto after = readBytes(h.project.render_config);
        const auto kept = Json::parse(after).at("rendering").at("target_planning").at("graphs").at("main_render");
        CHECK_FALSE(kept.contains("backend"));
        CHECK(kept.at("nodes").at("gone") == Json{{"serial", false}, {"isolate", false}, {"fuse", true}});
        CHECK(kept.at("resources").at("gone_resource") == Json{{"no_alias", true}});
        h.commit(h.service->restoreRenderDocuments({{"documents", Json::array({rd1Document("passes/main.json", renderConfigSourceDigest(after), before)})}}));
        CHECK(readBytes(h.project.render_config) == before);
        CHECK(Json::parse(before).at("rendering").at("target_planning").at("graphs").at("main_render") == original);
        h.commit(h.service->restoreRenderDocuments({{"documents", Json::array({rd1Document("passes/main.json", renderConfigSourceDigest(before), after)})}}));
        CHECK(readBytes(h.project.render_config) == after);
    }
}
TEST_CASE("RC-6b1 D2 policy batch CAS preflight and one document round trip", "[render-config-editor][rc6b1][rc6-d2-rpc]") {
    Rc5bHarness h;
    const auto before = readBytes(h.project.render_config);
    Json request{{"base_source_digest", h.context().at("source_digest").at("hex")}, {"graph", "main_render"},
        {"backend", {{"value", "materialized"}, {"strength", "require"}}},
        {"nodes", {{"forward_opaque", {{"scope_break", true}, {"scope_join", false}}}}},
        {"resources", {{"g_emissive", {{"representation", "materialized"}}}}}};
    SECTION("CAS rejects the entire request") {
        request["base_source_digest"] = std::string(64, '0');
        CHECK(rd1Error(h.service->editTargetPlanning(request)).at("code") == "external_modification");
        CHECK(readBytes(h.project.render_config) == before);
    }
    SECTION("unrepresentable policy applies nothing") {
        request["nodes"]["forward_opaque"]["scope_break"] = false;
        request["nodes"]["forward_opaque"]["scope_join"] = true;
        request["nodes"]["forward_opaque"]["isolate"] = true;
        h.runtime.validate_candidate = [&](const RenderConfigCandidateDocumentSet &documents) {
            vf1CompileAsRenderer(documents, h.resolver);
        };
        const auto accepted = h.service->editTargetPlanning(request);
        REQUIRE(accepted.at("status") == "accepted");
        h.service->commitPending();
        const auto result = h.service->getResult({{"ticket", accepted.at("ticket")}});
        CHECK(result.at("committed") == false);
        CHECK(result.at("error").at("code") == "target_planning.requirement_unsatisfied@1");
        CHECK(readBytes(h.project.render_config) == before);
    }
    SECTION("single edit undo redo restores all keys") {
        h.commit(h.service->editTargetPlanning(request));
        const auto after = readBytes(h.project.render_config);
        const auto graph = Json::parse(after).at("rendering").at("target_planning").at("graphs").at("main_render");
        CHECK(graph.at("backend") == request.at("backend")); CHECK(graph.at("nodes") == request.at("nodes")); CHECK(graph.at("resources") == request.at("resources"));
        h.commit(h.service->restoreRenderDocuments({{"documents", Json::array({rd1Document("passes/main.json", renderConfigSourceDigest(after), before)})}}));
        CHECK(readBytes(h.project.render_config) == before);
        h.commit(h.service->restoreRenderDocuments({{"documents", Json::array({rd1Document("passes/main.json", renderConfigSourceDigest(before), after)})}}));
        CHECK(readBytes(h.project.render_config) == after);
    }
}
}
