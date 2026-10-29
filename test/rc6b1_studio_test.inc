namespace PelicanStudio {
TEST_CASE("RC-6b1 fix authored suffixes survive Pin checks Freeze and inheritance", "[devstudio][frame-plan][rc6b1][rc6-fix-names]") {
    application();
    EditingFixture fixture;
    fixture.open();
    for (const auto *suffix : {"#xr", "#preview", "#flat"}) {
        for (const bool appended : {false, true}) {
            CAPTURE(suffix, appended);
            const std::string graph = std::string{"main"} + suffix;
            auto wire = fixture.wire;
            wire["graph_variant"] = {{"name", appended ? "xr" : "flat"}, {"name_suffix", appended ? "#xr" : ""}};
            auto model = buildFramePlanModel(wire.dump());
            model.graph = graph + (appended ? "#xr" : "");
            REQUIRE(model.nodes.size() > 2);
            model.nodes[1].authored_name = "T";
            model.nodes[2].authored_name = std::string{"T"} + suffix;
            auto author = framePlanAuthoringFromContext(mixedAuthoringContext(model.graph));
            author.planning_catalog[graph]["nodes"] = {"T", model.nodes[2].authored_name};
            author.planning_root["rendering"]["target_planning"]["graphs"][graph] = {
                {"backend", planningRequire("materialized")},
                {"nodes", {{model.nodes[2].authored_name, {{"serial", planningRequire(true)}}}}}};
            REQUIRE_FALSE(model.backend_candidates.empty());
            const auto pin = framePlanBackendPinRequest(author, model, model.selected_backend_candidate);
            REQUIRE(pin.sendable());
            CHECK(pin.params.at("graph") == graph);
            CHECK(framePlanBackendUnpinRequest(author, model).params.at("graph") == graph);
            CHECK(framePlanPlanningCheck(author, model, "node", model.nodes[2].name, "serial").root);
            const auto freeze = framePlanPhysicalFreezeRequest(author, model);
            REQUIRE(freeze.sendable());
            CHECK(freeze.params.at("graph") == graph);
            CHECK(freeze.params.at("nodes").size() == 2);
            CHECK(freeze.params.at("nodes").contains("T"));
            CHECK(freeze.params.at("nodes").contains(model.nodes[2].authored_name));
            const auto check = graphTargetPlanningParams(author.source_digest, framePlanAuthorName(model), "node",
                framePlanAuthorNodeName(model, model.nodes[2].name), "serial", true);
            CHECK(check.at("graph") == graph);
            CHECK(check.at("nodes").contains(model.nodes[2].authored_name));
            CHECK(framePlanPhysicalRemoveRequest(author, model).params.at("graph") == graph);
        }
    }
}

TEST_CASE("RC-6b1 fix Studio Freeze sends only catalog resources", "[devstudio][frame-plan][rc6b1][rc6-fix-resources]") {
    application();
    EditingFixture fixture;
    fixture.open();
    const auto model = buildFramePlanModel(fixture.wire.dump());
    auto author = framePlanAuthoringFromContext(mixedAuthoringContext(model.graph));
    author.planning_catalog[model.graph]["nodes"] = Json::array();
    author.planning_catalog[model.graph]["resources"] = Json::array();
    const auto request = framePlanPhysicalFreezeRequest(author, model);
    REQUIRE(request.sendable());
    CHECK_FALSE(request.params.contains("resources"));
}

TEST_CASE("RC-6b1 D4 effective inherited checks invert root require and restore inheritance", "[devstudio][frame-plan][rc6b1][rc6-d4]") {
    application();
    EditingFixture fixture;
    const std::string resource = "wp343_mixed_a_output";
    const auto part = Json{{"target", resource}, {"key", "no_alias"}, {"value", true}, {"strength", "prefer"}, {"status", "adopted"},
        {"origin", {{"kind", "part"}, {"document", "feature://scratch"}, {"pointer", "/target_planning/resources/scratch/no_alias"}}}};
    fixture.wire["physical_target_plan"]["policy"] = Json::array({part});
    fixture.open();
    auto *resources = fixture.widget.findChild<QTreeWidget *>(QStringLiteral("pelican.framePlanResources"));
    REQUIRE(resources != nullptr);
    auto openMenu = [&](const std::function<void(QMenu &)> &inspect) {
        auto rows = resources->findItems(QString::fromStdString(resource), Qt::MatchExactly, 0);
        REQUIRE(rows.size() == 1);
        for (auto *tab : fixture.widget.findChildren<QTabWidget *>()) if (tab->indexOf(resources) >= 0) tab->setCurrentWidget(resources);
        resources->setCurrentItem(rows.front());
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &fixture.widget, [&] {
            for (auto *surface : QApplication::topLevelWidgets()) if (auto *menu = qobject_cast<QMenu *>(surface); menu && menu->isVisible()) {
                timer.stop(); inspect(*menu); menu->close();
            }
        });
        timer.start(0);
        Q_EMIT resources->customContextMenuRequested(resources->visualItemRect(rows.front()).center());
    };
    openMenu([&](QMenu &menu) {
        auto *action = fixture.widget.findChild<QAction *>(QStringLiteral("pelican.framePlan.planning.noAlias"));
        // Public object name uses no_alias in older panels; locate the actual menu entry too.
        if (!action) for (auto *entry : menu.actions()) if (entry->text().startsWith(QStringLiteral("No alias"))) action = entry;
        REQUIRE(action != nullptr);
        CHECK(action->isChecked()); CHECK(action->text().contains(QStringLiteral("inherited")));
        CHECK(action->toolTip().contains(QStringLiteral("feature://scratch")));
        action->trigger();
    });
    REQUIRE(fixture.recorder.submits == 1);
    CHECK(sentParams(fixture.recorder).at("resources").at(resource).at("no_alias") == Json{{"value", false}, {"strength", "require"}});
    commitSubmitted(fixture);
    auto context = mixedAuthoringContext(fixture.graph);
    context["planning_root"]["rendering"]["target_planning"]["graphs"][fixture.graph]["resources"][resource]["no_alias"] = {{"value", false}, {"strength", "require"}};
    fixture.recorder.result(fixture.recorder.context_id, QByteArray::fromStdString(context.dump()));
    auto overridden = part; overridden["status"] = "overridden";
    auto adopted = part; adopted["origin"] = {{"kind", "root"}, {"document", "root"}, {"pointer", "/rendering"}}; adopted["value"] = false;
    fixture.wire["physical_target_plan"]["policy"] = Json::array({overridden, adopted});
    fixture.widget.receiveResult(QByteArray::fromStdString(fixture.wire.dump()));
    openMenu([&](QMenu &menu) {
        QAction *inherit = nullptr;
        for (auto *entry : menu.actions()) if (entry->objectName() == QStringLiteral("pelican.framePlan.planning.inherit.no_alias")) inherit = entry;
        REQUIRE(inherit != nullptr); inherit->trigger();
    });
    REQUIRE(fixture.recorder.submits == 2);
    CHECK(sentParams(fixture.recorder).at("resources").at(resource).at("no_alias").is_null());
    commitSubmitted(fixture);
    context["planning_root"] = Json::object();
    fixture.recorder.result(fixture.recorder.context_id, QByteArray::fromStdString(context.dump()));
    fixture.wire["physical_target_plan"]["policy"] = Json::array({part});
    fixture.widget.receiveResult(QByteArray::fromStdString(fixture.wire.dump()));
    auto model = buildFramePlanModel(fixture.wire.dump());
    auto author = framePlanAuthoringFromContext(context);
    CHECK(framePlanPlanningCheck(author, model, "resource", resource, "no_alias").checked);
    auto infeasible = part; infeasible["status"] = "infeasible_prefer";
    fixture.wire["physical_target_plan"]["policy"] = Json::array({infeasible});
    model = buildFramePlanModel(fixture.wire.dump());
    auto state = framePlanPlanningCheck(author, model, "resource", resource, "no_alias");
    CHECK_FALSE(state.checked); CHECK(state.reason.find("infeasible_prefer") != std::string::npos);
    const auto node = model.nodes.front().name;
    author.planning_catalog[framePlanAuthorName(model)]["engine_isolates"][framePlanAuthorNodeName(model, node)] = "graph scope: engine requires isolate";
    state = framePlanPlanningCheck(author, model, "node", node, "isolate");
    CHECK(state.checked); CHECK_FALSE(state.enabled); CHECK(state.reason.find("graph scope") != std::string::npos);
    auto fold = part; fold["origin"]["kind"] = "root"; fold["origin"]["pointer"] = "/fold_groups/my_fold/folded/decisions/no_alias";
    fixture.wire["physical_target_plan"]["policy"] = Json::array({fold});
    state = framePlanPlanningCheck(author, buildFramePlanModel(fixture.wire.dump()), "resource", resource, "no_alias");
    CHECK(state.checked); CHECK_FALSE(state.enabled); CHECK(state.reason.find("my_fold") != std::string::npos); CHECK(state.reason.find("fold group") != std::string::npos);
}
}
namespace PelicanStudio {
TEST_CASE("RC-6b1 D2 real bloom loop expansion reports every skipped node", "[devstudio][frame-plan][rc6b1][rc6-d2-loop]") {
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS && PELICAN_RUNTIME_SHADER_COMPILER
    auto root = Json::parse(loadEngineDocument("engine://graphs/bloom_loop.json"));
    root["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
                         {"features", Json::array({"engine://features/hdr.json"})}};
    auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{.authored_config = root, .source_name = "rc6b1_loop.json"},
        Pelican::RenderEnvironmentCapabilities{.runtime_shader_compiler_enabled = true},
        Pelican::RenderPipelineResolveDependencies{.load_feature_json = loadEngineDocument, .load_pipeline_json = loadEngineDocument});
    const auto model = buildFramePlanModel(resolvedFramePlan(std::move(resolved), "rc6b1_loop").dump());
    auto author = framePlanAuthoringFromContext(mixedAuthoringContext(model.graph));
    auto &names = author.planning_catalog[framePlanAuthorName(model)]["nodes"];
    names = Json::array();
    std::size_t count = 0;
    for (const auto &node : model.nodes) {
        names.push_back(node.name);
        if (node.loop_origin) ++count;
    }
    REQUIRE(count > 1);
    const auto request = framePlanPhysicalFreezeRequest(author, model);
    REQUIRE(request.sendable());
    for (const auto &node : model.nodes) if (node.loop_origin) CHECK_FALSE(request.params.at("nodes").contains(node.name));
    CHECK(request.planning_info == std::to_string(count) + " nodes inside loops are not frozen.");
    CHECK(request.notice.find(request.planning_info) != std::string::npos);
#else
    SKIP("standard render algorithms and runtime compiler required");
#endif
}
}
