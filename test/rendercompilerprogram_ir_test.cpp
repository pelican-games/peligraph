// RC-3c1b: the production compiler program (the one RegistrationStages::prepare
// runs, with the variant requests the registration makes: ImGui composed into
// flat, the request-local preview last) reads each completed revision once
// (parseRenderGraph), hands the provider chain's IR on, finalizes the last
// revision once, and refuses by name where an input is first refused.
#include "rendercompilerprogram_ir_test_support.hpp"
#include "../src/core/renderingpass/passdefinitionfromir.hpp"
#include "../src/project/passshaderdefines.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>

namespace Pelican {

TEST_CASE("RC-3c1b B1 the production compiler reads each completed revision once",
          "[rc3c1b][render-compiler][parse-count]") {
    // 1 + T + S per runtime variant (revision 0, each transform's response,
    // each spliced replacement), 1 for the preview; the number moves with the
    // providers as the formula says.
    for (const auto variants : {Variants::flat, Variants::flat_and_preview, Variants::flat_xr_and_preview}) {
        for (const int transforms : {0, 1, 2}) {
            for (const bool subgraph : {false, true}) {
                DYNAMIC_SECTION("variants " << static_cast<int>(variants) << " transforms " << transforms
                                            << " subgraph " << subgraph) {
                    Project project;
                    Providers providers;
                    const auto before = renderGraphParseCount();
                    const auto output = compile(project, providers, section(transforms, subgraph), variants);
                    const auto parses = renderGraphParseCount() - before;
                    const auto preview = variants == Variants::flat ? 0u : 1u;
                    const auto expected = runtimeVariantsOf(variants) *
                                              static_cast<std::size_t>(1 + transforms + (subgraph ? 1 : 0)) +
                                          preview;
                    CHECK(parses == expected);
                    for (const auto &variant : output.variants) CHECK_FALSE(variant.unavailable);
                }
            }
        }
    }
    SECTION("the preview alone (data_only) reads its composition once") {
        Project project;
        Providers providers;
        const auto before = renderGraphParseCount();
        const auto output = compile(project, providers, section(2, true), Variants::preview);
        CHECK(renderGraphParseCount() - before == 1);
        REQUIRE(output.variants.size() == 1);
        REQUIRE(output.variants.front().render_graph != nullptr);
        CHECK(output.variants.front().render_graph->graphs.at(0).name == "main");
    }
}

TEST_CASE("RC-3c1b B1 each provider receives the revision the previous response made",
          "[rc3c1b][render-compiler][provider-chain]") {
    // Two transforms that change the graph and a replacement: the second
    // transform's inputs are the first's response read once (its normalized
    // config, its logical graphs and their fingerprint), and the replacement
    // reads the region as the second transform left it.
    Project project;
    Providers providers;
    const auto output = compile(project, providers, section(2, true), Variants::flat);
    const auto after_first = revisionOf(providers.first.response);
    CHECK(providers.second.received_config == after_first.config);
    CHECK(providers.second.received_graphs == after_first.graphs);
    CHECK(providers.second.received_fingerprint == after_first.fingerprint);
    // Not revision 0: the first transform's pass is in what the second read.
    CHECK(providers.second.received_graphs.find("t1_pass") != std::string::npos);
    CHECK(providers.first.received_graphs.find("t1_pass") == std::string::npos);
    // The replacement reads tone as the second transform left it.
    const auto after_second = revisionOf(providers.second.response);
    const auto authored = Json::parse(providers.subgraph.received_authored);
    CHECK(authored.at(0).at("input") == Json::array({"t2_scratch"}));
    const auto logical = TestSupport::logicalGraphsOf(after_second.ir);
    CHECK(providers.subgraph.received_fingerprint ==
          makeTaggedRegionContract(logical.at(0), "rc3c1b.tone").fingerprint);
    // The final revision: the replacement's passes, the provenance of all
    // three on the graph (the provider read none).
    const auto &flat = variantOf(output, RenderPipelineGraphVariant::flat);
    REQUIRE(flat.render_graph != nullptr);
    const auto &graph = flat.render_graph->graphs.at(0);
    CHECK(graph.graph_transforms.size() == 2);
    CHECK(graph.subgraph_replacements.size() == 1);
    CHECK(graph.render_strategy.has_value());
    CHECK(providers.second.received_graphs.find("graph_transforms\":[{") == std::string::npos);
    const auto order = framePlanOrder(flat.frame_plans.at("main"));
    for (const auto *name : {"t1_pass", "t2_pass", "tone_a", "tone_b"}) {
        INFO(name);
        CHECK(std::find(order.begin(), order.end(), name) != order.end());
    }
}

TEST_CASE("RC-3c1b B2 frame_node_order names the node a provider added",
          "[rc3c1b][render-compiler][frame-node-order]") {
    // The provider puts x at the head of main; the authored side is
    // independent of it. Two orders, both agreeing with the dependencies, give
    // two plans (side first is not the declaration order).
    auto authored = section(1, false);
    authored["render_targets"].push_back(image("side_out"));
    authored["rendering_passes"][0]["passes"].push_back(fullscreen("side", {}, "side_out"));
    const auto order_of = [&](const std::vector<std::string> &order, bool with_provider) {
        auto rendering = authored;
        rendering["frame_node_order"] = {{"main", order}};
        if (!with_provider) rendering.erase("graph_transforms");
        Project project;
        Providers providers;
        providers.first.behavior = TransformBehavior::independent;
        const auto output = compile(project, providers, rendering, Variants::flat);
        return framePlanOrder(variantOf(output, RenderPipelineGraphVariant::flat).frame_plans.at("main"));
    };
    const auto position = [](const std::vector<std::string> &order, std::string_view name) {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };
    const auto side_first = order_of({"side", "x"}, true);
    const auto x_first = order_of({"x", "side"}, true);
    CHECK(position(side_first, "side") < position(side_first, "x"));
    CHECK(position(x_first, "x") < position(x_first, "side"));
    CHECK(side_first != x_first);
    // Control: without the provider there is no x, and the order is refused
    // by name.
    TestSupport::requireNamedRefusal([&] { (void)order_of({"side", "x"}, false); },
                                     "render_graph.frame_node_order_unknown@1", {{"graph", "main"}, {"node", "x"}});
}

TEST_CASE("RC-3c1b B4 the production compiler refuses by name where an input is first refused",
          "[rc3c1b][render-compiler][refusal]") {
    const auto refused = [](const Json &rendering, std::string_view code, const Json &subject,
                            TransformBehavior behavior = TransformBehavior::chain) {
        Project project;
        Providers providers;
        providers.first.behavior = behavior;
        TestSupport::requireNamedRefusal(
            [&] { (void)compile(project, providers, rendering, Variants::flat_and_preview); }, code, subject);
    };
    auto &&base = section(0, false);
    SECTION("a cycle a provider's response makes") {
        refused(section(1, false), "render_graph.cycle@1", {{"graph", "main"}}, TransformBehavior::cycle);
    }
    SECTION("frame_node_order naming a graph no variant has") {
        auto rendering = base;
        rendering["frame_node_order"] = {{"ghost", Json::array()}};
        refused(rendering, "render_graph.frame_node_order_unknown@1", {{"graph", "ghost"}});
    }
    SECTION("frame_node_order naming a node the graph has not") {
        auto rendering = base;
        rendering["frame_node_order"] = {{"main", Json::array({"draw", "ghost"})}};
        refused(rendering, "render_graph.frame_node_order_unknown@1", {{"graph", "main"}, {"node", "ghost"}});
    }
    SECTION("a pass input nothing writes before it") {
        auto rendering = base;
        rendering["render_targets"].push_back(image("never"));
        rendering["rendering_passes"][0]["passes"][2]["input"] = Json::array({"post", "never"});
        refused(rendering, "render_graph.input_not_produced@1", {{"node", "present"}, {"resource", "never"}});
    }
    SECTION("a read of an undeclared resource") {
        auto rendering = base;
        rendering["rendering_passes"][0]["passes"][2]["input"] = Json::array({"post", "ghost"});
        refused(rendering, "render_graph.unknown_resource@1", {{"node", "present"}, {"resource", "ghost"}});
    }
    SECTION("a history read of an image without history") {
        auto rendering = base;
        rendering["rendering_passes"][0]["passes"][2]["input"] = Json::array({"post", "scene@history"});
        refused(rendering, "render_graph.unknown_history_resource@1", {{"node", "present"}, {"resource", "scene"}});
    }
    SECTION("after naming no node") {
        auto rendering = base;
        rendering["rendering_passes"][0]["passes"][2]["after"] = Json::array({"tone", "nowhere"});
        refused(rendering, "render_graph.order_reference_unknown@1", {{"node", "present"}, {"reference", "nowhere"}});
    }
    SECTION("a node after itself") {
        auto rendering = base;
        rendering["rendering_passes"][0]["passes"][2]["after"] = Json::array({"present"});
        refused(rendering, "render_graph.self_dependency@1", {{"node", "present"}});
    }
    SECTION("an authored cycle") {
        auto rendering = base;
        rendering["rendering_passes"][0]["passes"][0]["after"] = Json::array({"present"});
        refused(rendering, "render_graph.cycle@1", {{"graph", "main"}});
    }
    SECTION("two unordered writers of one buffer") {
        auto rendering = base;
        rendering["buffers"] = Json::array({Json{{"name", "shared"}}});
        const auto writer = [](const std::string &name) {
            return Json{{"name", name},
                        {"shader", "engine://shaders/compute/fixture"},
                        {"writes", Json::array({"shared"})},
                        {"dispatch", {{"groups", Json::array({1, 1, 1})}}}};
        };
        rendering["compute_tasks"] = Json::array({writer("write_a"), writer("write_b")});
        refused(rendering, "render_graph.ambiguous_writes@1", {{"resource", "shared"}});
    }
    SECTION("a provider's response with two passes of one name") {
        // An authored duplicate is refused by the composition first (its own
        // sentence); a provider's response reaches the IR.
        refused(section(1, false), "render_graph.duplicate_node@1", {{"node", "draw"}},
                TransformBehavior::duplicate);
    }
    SECTION("an acceleration structure nobody declares") {
        auto rendering = base;
        rendering["buffers"] = Json::array({Json{{"name", "hits"}}});
        rendering["compute_tasks"] = Json::array(
            {Json{{"name", "trace"},
                  {"shader", "engine://shaders/compute/fixture"},
                  {"writes", Json::array({"hits"})},
                  {"dispatch", {{"groups", Json::array({1, 1, 1})}}},
                  {"ray_query", {{"acceleration_structure", "missing"}}}}});
        refused(rendering, "render_graph.acceleration_structure_unknown@1",
                {{"node", "trace"}, {"acceleration_structure", "missing"}});
    }
}

TEST_CASE("RC-3c1b ray_task_acceleration_structures may name a task a provider adds",
          "[rc3c1b][render-compiler][finalize]") {
    // The table is a protected control field: a transform cannot change it,
    // but may add the task it names; each revision holds its form and the
    // final revision resolves it. Without the provider no task has the name:
    // refused in the finalize by name. (The accepted case, with the table's
    // selection in the final IR, needs the device's ray query:
    // rendercompilerprogram_device_test.)
    auto rendering = section(1, false);
    rendering["ray_task_acceleration_structures"] = {{"x_trace", "scene"}};
    rendering.erase("graph_transforms");
    Project project;
    Providers providers;
    providers.first.behavior = TransformBehavior::ray_task;
    TestSupport::requireNamedRefusal([&] { (void)compile(project, providers, rendering, Variants::flat); },
                                     "render_graph.compute_task_invalid@1",
                                     {{"node", "x_trace"}, {"field", "ray_task_acceleration_structures"},
                                      {"reason", "unknown_task"}});
}

TEST_CASE("RC-3c1b an unknown acceleration structure is refused by the author's task name in every variant",
          "[rc3c1b][render-compiler][acceleration-structure]") {
    auto rendering = section(0, false);
    rendering["buffers"] = Json::array({Json{{"name", "hits"}}});
    rendering["compute_tasks"] = Json::array(
        {Json{{"name", "trace"},
              {"shader", "engine://shaders/compute/fixture"},
              {"writes", Json::array({"hits"})},
              {"dispatch", {{"groups", Json::array({1, 1, 1})}}},
              {"ray_query", {{"acceleration_structure", "missing"}}}}});
    std::vector<Variants> variant_sets{Variants::flat};
#if PELICAN_WITH_OPENXR
    variant_sets.push_back(Variants::xr);
#endif
    for (const auto variants : variant_sets) {
        DYNAMIC_SECTION("variants " << static_cast<int>(variants)) {
            Project project;
            Providers providers;
            // XR names the task trace#xr after its suffix; the refusal names
            // the task as the author wrote it.
            TestSupport::requireNamedRefusal(
                [&] { (void)compile(project, providers, rendering, variants); },
                "render_graph.acceleration_structure_unknown@1",
                {{"node", "trace"}, {"acceleration_structure", "missing"}});
        }
    }
}

TEST_CASE("RC-3c1b frame_node_order orders two writers a provider's revision leaves unordered",
          "[rc3c1b][render-compiler][frame-node-order]") {
    // Two tasks write one buffer with no order between them; the author's
    // frame_node_order orders them. A revision before the final one (a
    // transform's input and response) holds them unordered; the final plan
    // judges the writes after the order.
    auto rendering = section(0, false);
    rendering["buffers"] = Json::array({Json{{"name", "selection"}}});
    const auto writer = [](const std::string &name) {
        return Json{{"name", name},
                    {"shader", "engine://shaders/compute/fixture"},
                    {"writes", Json::array({"selection"})},
                    {"dispatch", {{"groups", Json::array({1, 1, 1})}}}};
    };
    rendering["compute_tasks"] = Json::array({writer("select"), writer("select_again")});
    rendering["frame_node_order"] = {{"main", Json::array({"select_again", "select"})}};
    const auto plan_of = [](const Json &authored) {
        Project project;
        Providers providers;
        const auto output = compile(project, providers, authored, Variants::flat);
        return framePlanOrder(variantOf(output, RenderPipelineGraphVariant::flat).frame_plans.at("main"));
    };
    const auto position = [](const std::vector<std::string> &order, std::string_view name) {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };
    auto identity = rendering;
    identity["graph_transforms"] = Json::array(
        {Json{{"name", "rc3c1b.identity"}, {"provider", std::string{builtinLogicalGraphTransformProvider}}}});
    for (const auto *authored : {&rendering, &identity}) {
        const auto order = plan_of(*authored);
        CHECK(position(order, "select_again") < position(order, "select"));
    }
    // Control: without the order the final plan refuses the writers by name.
    auto unordered = identity;
    unordered.erase("frame_node_order");
    TestSupport::requireNamedRefusal([&] { (void)plan_of(unordered); }, "render_graph.ambiguous_writes@1",
                                     {{"resource", "selection"}});
}

TEST_CASE("RC-3c1b the registration's requests compose ImGui into flat as the launch configuration says",
          "[rc3c1b][render-compiler][imgui]") {
    // The requests are the registration's (makeRenderGraphVariantRequests);
    // the compile is the program prepare runs. ImGui on: the flat variant's
    // final IR and FramePlan have the ImGui pass; off: neither has it. One
    // parse per revision either way.
    const auto compiled = [](bool imgui) {
        Project project;
        Providers providers;
        const auto before = renderGraphParseCount();
        auto output = compile(project, providers, section(1, true), Variants::flat_and_preview, imgui);
        const auto parses = renderGraphParseCount() - before;
        const auto &flat = variantOf(output, RenderPipelineGraphVariant::flat);
        bool in_ir = false;
        for (const auto &node : flat.render_graph->graphs.at(0).nodes)
            in_ir = in_ir || (node.name == "imgui_pass" && node.shape == RenderNodeShape::imgui);
        const auto order = framePlanOrder(flat.frame_plans.at("main"));
        const bool in_plan = std::find(order.begin(), order.end(), "imgui_pass") != order.end();
        return std::tuple{parses, in_ir, in_plan};
    };
#if PELICAN_WITH_IMGUI
    CHECK(compiled(true) == std::tuple{std::size_t{1 + 1 + 1 + 1}, true, true});
#else
    CHECK(compiled(true) == std::tuple{std::size_t{1 + 1 + 1 + 1}, false, false});
#endif
    CHECK(compiled(false) == std::tuple{std::size_t{1 + 1 + 1 + 1}, false, false});
}

TEST_CASE("RC-3c1b a format class without a format reaches the preview's IR unresolved",
          "[rc3c1b][render-compiler][preview][format-class]") {
    // resolver_version 2: the runtime package resolves each class to a
    // format; the preview composes without resolving it. Its IR takes the
    // image with its format unresolved, so the preview stays available (the
    // planning parse it replaced never read a format).
    // A fixed-size image as well (64x64: its snapshot bytes would need the
    // format), with its format and with its class only.
    for (const bool class_only : {false, true}) {
        DYNAMIC_SECTION((class_only ? "class only" : "with a format")) {
            auto rendering = section(0, false);
            rendering["resolver_version"] = 2;
            auto fixed = image("fixed");
            fixed["width"] = 64;
            fixed["height"] = 64;
            rendering["render_targets"].push_back(fixed);
            for (auto &target : rendering["render_targets"]) {
                target["format_class"] = "scene";
                if (class_only) target.erase("format");
            }
            Project project;
            Providers providers;
            const auto output = compile(project, providers, rendering, Variants::flat_and_preview);
            const auto &flat = variantOf(output, RenderPipelineGraphVariant::flat);
            CHECK_FALSE(flat.unavailable);
            // The runtime's images have their resolved formats.
            for (const auto &resolved : flat.render_graph->images) CHECK_FALSE(resolved.format_unresolved);
            const auto &preview = variantOf(output, RenderPipelineGraphVariant::preview);
            CHECK_FALSE(preview.unavailable);
            REQUIRE(preview.render_graph != nullptr);
            CHECK(std::any_of(preview.render_graph->images.begin(), preview.render_graph->images.end(),
                              [](const ImageResource &declared) { return declared.format_unresolved; }) ==
                  class_only);
        }
    }
}

TEST_CASE("RC-3c1b a compute task's persistent image is not a pass input; a pass input is",
          "[rc3c1b][render-compiler][input-not-produced]") {
    // The same compile: a compute task reads and writes a persistent image
    // (its last contents) and is accepted; a pass reading an image nothing
    // writes before it is refused by name.
    auto rendering = section(0, false);
    auto state = image("state", Json::array({"STORAGE", "SAMPLED"}));
    state["lifetime"] = "persistent";
    rendering["render_targets"].push_back(state);
    rendering["compute_tasks"] = Json::array(
        {Json{{"name", "evolve"},
              {"shader", "engine://shaders/compute/fixture"},
              {"reads", Json::array({"state"})},
              {"writes", Json::array({"state"})},
              {"dispatch", {{"groups", Json::array({1, 1, 1})}}}}});
    {
        Project project;
        Providers providers;
        const auto output = compile(project, providers, rendering, Variants::flat);
        CHECK(variantOf(output, RenderPipelineGraphVariant::flat).frame_plans.contains("main"));
    }
    rendering["render_targets"].push_back(image("never"));
    rendering["rendering_passes"][0]["passes"][2]["input"] = Json::array({"post", "never"});
    Project project;
    Providers providers;
    TestSupport::requireNamedRefusal(
        [&] { (void)compile(project, providers, rendering, Variants::flat); },
        "render_graph.input_not_produced@1", {{"node", "present"}, {"resource", "never"}});
}

namespace {


NodeIR &sharedNode(GraphIR &graph, std::string_view name) {
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const NodeIR &node) {
        return node.scope == NodeDeclarationScope::shared && node.name == name;
    });
    REQUIRE(found != graph.nodes.end());
    return *found;
}

} // namespace

TEST_CASE("RC-3c2b1 the completion refuses by name an IR whose compute tasks it cannot make whole",
          "[rc3c2b1][render-compiler][compute]") {
    // Two config tasks: make_args (groups) and run (indirect from args at
    // offset 12). Whatever a program returns, the compute tasks are made
    // from its IR: a task the name index does not hold, two copies that do
    // not run the same, an indirect command off its 4-byte alignment are
    // refused there by name, before anything registers. Each with its
    // control (the same program, an IR that holds).
    auto rendering = section(0, false);
    rendering["buffers"] = Json::array({Json{{"name", "args"}, {"size", 32}, {"command_layout", "compute_dispatch"}},
                                        Json{{"name", "data"}, {"size", 16}}});
    rendering["compute_tasks"] = Json::array(
        {Json{{"name", "make_args"},
              {"shader", "engine://shaders/compute/fixture"},
              {"writes", Json::array({"args"})},
              {"dispatch", {{"groups", Json::array({1, 1, 1})}}}},
         Json{{"name", "run"},
              {"shader", "engine://shaders/compute/fixture"},
              {"writes", Json::array({"data"})},
              {"dispatch", {{"indirect", {{"buffer", "args"}, {"offset", 12}}}}}}});
    bool copy_graph = false;
    const auto compiled = [&](std::function<void(RenderGraphIR &)> edit) {
        EditingProgram program;
        program.edit = std::move(edit);
        program.copy_graph = copy_graph;
        Project project;
        Providers providers;
        return compile(project, providers, rendering, Variants::flat, true, nullptr, &program);
    };
    const auto refused = [&](std::function<void(RenderGraphIR &)> edit, const Json &detail) {
        TestSupport::requireNamedRefusal([&] { (void)compiled(std::move(edit)); },
                                         "render_graph.compute_task_invalid@1", detail);
    };
    SECTION("the name index holds every copy") {
        const auto control = compiled([](RenderGraphIR &) {});
        REQUIRE(control.compute_task_definitions.size() == 1);
        CHECK(control.compute_task_definitions.front().size() == 2);
        refused([](RenderGraphIR &ir) {
                    ir.shared_compute_tasks.clear();
                    ir.authored_shared_compute_tasks.clear();
                },
                {{"node", "make_args"}, {"field", "shared_compute_tasks"}, {"reason", "unindexed"}, {"graph", "main"}});
        // Neither index names them and the nodes claim another scope: a
        // compute node that is not the config's (shared) is refused, not
        // passed over.
        refused([](RenderGraphIR &ir) {
                    ir.shared_compute_tasks.clear();
                    ir.authored_shared_compute_tasks.clear();
                    for (auto &node : ir.graphs.front().nodes)
                        if (node.shape == RenderNodeShape::compute) node.scope = NodeDeclarationScope::synthesized;
                },
                {{"node", "make_args"}, {"field", "shared_compute_tasks"}, {"reason", "not_shared"}, {"graph", "main"}});
        refused([](RenderGraphIR &ir) { ir.authored_shared_compute_tasks.pop_back(); },
                {{"field", "shared_compute_tasks"}, {"reason", "authored_names_mismatch"}});
        refused([](RenderGraphIR &ir) {
                    auto &nodes = ir.graphs.front().nodes;
                    nodes.erase(std::find_if(nodes.begin(), nodes.end(),
                                             [](const NodeIR &node) { return node.name == "run"; }));
                },
                {{"node", "run"}, {"field", "shared_compute_tasks"}, {"reason", "missing_copy"}, {"graph", "main"}});
    }
    SECTION("every graph's copy runs the same, whatever its edges") {
        copy_graph = true;
        const auto two_graphs = [](RenderGraphIR &ir) {
            REQUIRE(ir.graphs.size() == 2);
            sharedNode(ir.graphs.back(), "make_args").after.push_back("draw");
        };
        const auto control = compiled(two_graphs);
        REQUIRE(control.compute_task_definitions.size() == 1);
        CHECK(control.compute_task_definitions.front().size() == 2);
        refused(
            [&](RenderGraphIR &ir) {
                two_graphs(ir);
                std::get<ComputeNodeInfo>(sharedNode(ir.graphs.back(), "make_args").info).dispatch.groups_x = 8;
            },
            {{"node", "make_args"}, {"field", "shared_compute_tasks"}, {"reason", "copies_differ"}, {"graph", "copy"}});
    }
    SECTION("an indirect command is 4-byte aligned") {
        const auto offset = [](std::uint64_t value) {
            return [value](RenderGraphIR &ir) {
                std::get<ComputeNodeInfo>(sharedNode(ir.graphs.front(), "run").info).dispatch.indirect->offset = value;
            };
        };
        const auto aligned = compiled(offset(0));
        REQUIRE(aligned.compute_task_definitions.size() == 1);
        CHECK(aligned.compute_task_definitions.front().at(1).dispatch.indirect->offset == 0);
        refused(offset(2), {{"node", "run"},
                            {"field", "dispatch.indirect.offset"},
                            {"reason", "not_4_byte_aligned"},
                            {"offset", 2}});
    }
}


TEST_CASE("RC-3c2b2 completion derives optional pass state and refuses missing stages",
          "[rc3c2b2][render-compiler][passes]") {
    const auto bloom = Json::parse(engineResourceOrThrow("features/bloom.json"));
    Json state;
    for (const auto &entry : bloom.at("passes")) {
        const auto &pass = entry.contains("pass") ? entry.at("pass") : entry;
        if (pass.contains("raster_state")) { state = pass.at("raster_state"); break; }
    }
    REQUIRE(state.is_object());
    for (const bool authored : {false, true}) {
        DYNAMIC_SECTION("bloom state authored " << authored) {
            Project project;
            Providers providers;
            auto rendering = section(0, false);
            auto &passes = rendering["rendering_passes"][0]["passes"];
            auto tone = std::find_if(passes.begin(), passes.end(), [](const auto &pass) { return pass.at("name") == "tone"; });
            REQUIRE(tone != passes.end());
            if (authored) (*tone)["raster_state"] = state;
            else tone->erase("raster_state");
            const auto output = compile(project, providers, rendering, Variants::flat);
            const auto &variant = variantOf(output, RenderPipelineGraphVariant::flat);
            const auto definitions = renderingPassDefinitionsOf(*variant.render_graph, output.generation_resources,
                                                                generationRenderTargetNames(output.generation_resources));
            const auto &actual_passes = definitions.at(0).passes;
            const auto actual = std::find_if(actual_passes.begin(), actual_passes.end(), [](const auto &pass) { return pass.name == "tone"; });
            REQUIRE(actual != actual_passes.end());
            const auto &raster_state = actual->fullscreenInfo().raster_state;
            REQUIRE(raster_state.has_value() == authored);
            if (authored) {
                REQUIRE_FALSE(raster_state->color_attachments.empty());
                const auto &blend = raster_state->color_attachments.at(0).blend;
                CHECK(blend.enabled);
                CHECK(blend.color.source == MaterialOutputBlendFactor::one);
                CHECK(blend.color.destination == MaterialOutputBlendFactor::one);
            }
        }
    }
    // A compiler program can return typed IR directly. Common completion
    // must refuse a missing execution stage before registration exists.
    Project project;
    Providers providers;
    EditingProgram program;
    program.edit = [](RenderGraphIR &ir) {
        for (auto &node : ir.graphs.front().nodes) {
            if (node.name == "tone") std::get<FullscreenNodeInfo>(node.info).fragment_shader.reset();
        }
    };
    TestSupport::requireNamedRefusal([&] {
        (void)compile(project, providers, section(0, false), Variants::flat, true, nullptr, &program);
    }, "render_graph.pass_execution_invalid@1",
       {{"graph", "main"}, {"node", "tone"}, {"field", "shader.fragment"}, {"reason", "required"}});
}


TEST_CASE("RC-3c2b2 pass declarations separate shape from execution requirements",
          "[rc3c2b2][render-compiler][passes]") {
    const auto parsed = [](Json pass) {
        return parseRenderGraph(normalizeRenderingJson(Json{
            {"render_targets", Json::array({image("color"), Json{{"name", "depth"}, {"extent_scale", 1.0},
                {"format", "D32_SFLOAT"}, {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}}})},
            {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({pass})}}})}}),
            buildPassFieldOwnershipCapabilities());
    };
    const auto definition = [](const RenderGraphIR &ir) {
        const auto resources = resolveDeclaredGenerationResources(ir, {64, 64});
        return passDefinitionOf(ir, ir.graphs.at(0), ir.graphs.at(0).nodes.at(0), resources,
                                generationRenderTargetNames(resources));
    };
    auto pass = fullscreen("subject", {}, "color");
    SECTION("missing fullscreen stages remain valid planning input") {
        pass.erase("shader");
        const auto ir = parsed(pass);
        TestSupport::requireNamedRefusal([&] { (void)definition(ir); }, "render_graph.pass_execution_invalid@1",
            {{"graph", "main"}, {"node", "subject"}, {"field", "shader.vertex"}, {"reason", "required"}});
    }
    SECTION("missing raster draw remains valid planning input") {
        pass["type"] = "raster";
        const auto ir = parsed(pass);
        TestSupport::requireNamedRefusal([&] { (void)definition(ir); }, "render_graph.pass_execution_invalid@1",
            {{"node", "subject"}, {"field", "draw"}, {"reason", "required"}});
        pass["draw"] = {{"vertex_count", 3}};
        CHECK(definition(parsed(pass)).isGenericRaster());
    }
    SECTION("shape refusals name the field and node") {
        for (const auto &bad : std::vector<std::pair<std::string, Json>>{
                {"uses_light_data", "yes"}, {"push_constants", "unknown"},
                {"input_settings", Json::array({nullptr})}, {"input_sampling", Json::array({Json::object()})},
                {"clear_color", Json::array({0, 1, 2})},
                {"clear_colors", {{"not_an_output", Json::array({0, 0, 0, 1})}}}}) {
            auto invalid = pass;
            invalid[bad.first] = bad.second;
            TestSupport::requireNamedRefusal([&] { (void)parsed(invalid); }, "render_graph.node_field_invalid@1",
                {{"graph", "main"}, {"node", "subject"}, {"field", bad.first}});
        }
        pass["shader"]["fragment"] = "fixture.frag";
        TestSupport::requireNamedRefusal([&] { (void)parsed(pass); }, "render_graph.node_field_invalid@1",
            {{"node", "subject"}, {"field", "shader.fragment"}, {"reason", "explicit_extension"}});
    }
    SECTION("even empty named bindings replace an empty positional declaration") {
        pass["type"] = "material";
        pass.erase("shader");
        pass["input"] = Json::array();
        pass["output"]["depth"] = "depth";
        for (const auto *field : {"screen_inputs", "surface_resources", "material_resources"}) {
            auto invalid = pass;
            invalid[field] = Json::object();
            TestSupport::requireNamedRefusal([&] { (void)parsed(invalid); }, "render_graph.node_field_invalid@1",
                {{"node", "subject"}, {"field", field}, {"reason", "replaces_positional_input"}});
        }
    }
    SECTION("define details survive the parser guard") {
        pass["shader"]["defines"] = Json::array({"LOCAL=1", "PELICAN_PASS_TEXTURE_INPUT_COUNT=5"});
        try { (void)parsed(pass); FAIL("reserved define accepted"); }
        catch (const PassShaderDefineError &error) {
            CHECK(error.info().pass == "subject");
            CHECK(error.info().define == "PELICAN_PASS_TEXTURE_INPUT_COUNT=5");
            CHECK(error.info().name == "PELICAN_PASS_TEXTURE_INPUT_COUNT");
            CHECK(error.info().index == 1);
            CHECK(error.info().reason == passShaderDefineReserved);
        }
    }
    SECTION("auxiliary defaults and an explicit empty shader are distinct") {
        for (const auto *type : {"shadow_depth", "velocity", "picking"}) {
            pass["type"] = type;
            if (std::string_view{type} == "shadow_depth") pass["output"] = {{"color", nullptr}, {"depth", "depth"}};
            else pass["output"] = {{"color", Json::array({Json{{"target", "color"}}})}, {"depth", "depth"}};
            pass.erase("shader");
            const auto omitted = definition(parsed(pass));
            CHECK(omitted.shader_declaration_parsed);
            CHECK(omitted.declared_shader_refs.empty());
            const std::string stem = std::string{"engine://"} + type;
            const auto check_shader = [](const ShaderReference &shader, const std::string &ref, ShaderStage stage) {
                CHECK(shader.ref == ref);
                CHECK(shader.stage == stage);
            };
            const auto check_effective = [&](const PassDefinition &definition, bool explicit_refs, bool skinned) {
                const auto vertex = explicit_refs ? "engine://custom_aux" : stem;
                const auto fragment = explicit_refs ? "engine://custom_aux" : stem;
                const auto skin = skinned ? "engine://custom_skin" : stem + "_skinned";
                if (std::string_view{type} == "shadow_depth") {
                    check_shader(definition.shadowDepthInfo().vert_shader, vertex, ShaderStage::vertex);
                } else if (std::string_view{type} == "velocity") {
                    check_shader(definition.velocityInfo().vert_shader, vertex, ShaderStage::vertex);
                    check_shader(definition.velocityInfo().frag_shader, fragment, ShaderStage::fragment);
                    check_shader(definition.velocityInfo().skinned_vert_shader, skin, ShaderStage::vertex);
                } else {
                    check_shader(definition.pickingInfo().vert_shader, vertex, ShaderStage::vertex);
                    check_shader(definition.pickingInfo().frag_shader, fragment, ShaderStage::fragment);
                    check_shader(definition.pickingInfo().skinned_vert_shader, skin, ShaderStage::vertex);
                }
            };
            check_effective(omitted, false, false);
            pass["shader"] = {{"vertex", "engine://custom_aux"}};
            const bool shadow = std::string_view{type} == "shadow_depth";
            if (!shadow) pass["shader"]["fragment"] = "engine://custom_aux";
            const auto explicit_pair = definition(parsed(pass));
            check_effective(explicit_pair, true, false);
            REQUIRE(explicit_pair.declared_shader_refs.size() == (shadow ? 1 : 2));
            CHECK(explicit_pair.declared_shader_refs.front().ref == "engine://custom_aux");
            CHECK(explicit_pair.declared_shader_refs.front().stage == DeclaredShaderStage::vertex);
            if (!shadow) {
                CHECK(explicit_pair.declared_shader_refs.back().stage == DeclaredShaderStage::fragment);
                pass["shader"]["skinned_vertex"] = "engine://custom_skin";
                const auto explicit_skinned = definition(parsed(pass));
                check_effective(explicit_skinned, true, true);
                REQUIRE(explicit_skinned.declared_shader_refs.size() == 3);
                CHECK(explicit_skinned.declared_shader_refs.at(1).ref == "engine://custom_skin");
                CHECK(explicit_skinned.declared_shader_refs.at(1).stage == DeclaredShaderStage::skinned_vertex);
            }
            pass["shader"] = Json::object();
            const auto empty = parsed(pass);
            TestSupport::requireNamedRefusal([&] { (void)definition(empty); }, "render_graph.pass_execution_invalid@1",
                {{"node", "subject"}, {"field", "shader.vertex"}, {"reason", "required"}});
        }
    }
}

TEST_CASE("RC-3c2b2 D2 compiler entry preserves fullscreen and auxiliary refusals",
          "[rc3c2b2][render-compiler][passes][d2]") {
    const auto compiled = [](const Json &rendering) {
        Project project;
        Providers providers;
        return compile(project, providers, rendering, Variants::flat);
    };
    const auto base = section(0, false);
    REQUIRE(compiled(base).variants.size() == 1);
    struct Refusal {
        std::string label;
        std::function<void(Json &)> edit;
        std::string code;
        Json fields;
    };
    const std::string field_code = "render_graph.node_field_invalid@1";
    const std::string execution_code = "render_graph.pass_execution_invalid@1";
    const std::vector<Refusal> refusals{
        {"missing shader", [](Json &p) { p.erase("shader"); }, execution_code,
            {{"field", "shader.vertex"}, {"reason", "required"}}},
        {"missing vertex", [](Json &p) { p["shader"].erase("vertex"); }, execution_code,
            {{"field", "shader.vertex"}, {"reason", "required"}}},
        {"missing fragment", [](Json &p) { p["shader"].erase("fragment"); }, execution_code,
            {{"field", "shader.fragment"}, {"reason", "required"}}},
        {"shader not object", [](Json &p) { p["shader"] = "fullscreen"; }, field_code,
            {{"field", "shader"}, {"reason", "not_an_object"}}},
        {"shader stage not string", [](Json &p) { p["shader"]["fragment"] = 4; }, field_code,
            {{"field", "shader.fragment"}, {"reason", "requires_string"}}},
        {"explicit shader extension", [](Json &p) { p["shader"]["vertex"] = "fullscreen.vert.spv"; }, field_code,
            {{"field", "shader.vertex"}, {"reason", "explicit_extension"}, {"ref", "fullscreen.vert.spv"}}},
        {"unknown shader field", [](Json &p) { p["shader"]["typo"] = "ignored_before"; }, field_code,
            {{"field", "shader.typo"}, {"reason", "unknown_field"}}},
        {"push constants", [](Json &p) { p["push_constants"] = "unknown"; }, field_code,
            {{"field", "push_constants"}, {"reason", "unknown_value"}}},
        {"lighting type", [](Json &p) { p["uses_light_data"] = "true"; }, field_code,
            {{"field", "uses_light_data"}, {"reason", "requires_boolean"}}},
        {"sampling not array", [](Json &p) { p["input_sampling"] = "linear"; }, field_code,
            {{"field", "input_sampling"}, {"reason", "not_an_array"}}},
        {"sampling cardinality", [](Json &p) { p["input_sampling"] = Json::array(); }, field_code,
            {{"field", "input_sampling"}, {"reason", "image_input_count_mismatch"}}},
        {"sampling enum", [](Json &p) { p["input_sampling"] = Json::array({Json{{"filter", "invalid"}}}); }, field_code,
            {{"field", "input_sampling"}, {"reason", "invalid"}}},
        {"sampling unknown key", [](Json &p) { p["input_sampling"] = Json::array({Json{{"wrap", "repeat"}}}); }, field_code,
            {{"field", "input_sampling"}, {"reason", "invalid"}}},
        {"sampling and ports", [](Json &p) {
            p["resource_ports"] = {{"color", "scene"}};
            p["input_sampling"] = Json::array({Json::object()});
        }, field_code, {{"field", "input_sampling"}, {"reason", "replaced_by_resource_ports"}}},
        {"port missing edge", [](Json &p) { p["resource_ports"] = {{"color", "unknown_input"}}; }, field_code,
            {{"field", "resource_ports"}, {"reason", "invalid"}}},
        {"duplicate ports", [](Json &p) { p["resource_ports"] = {
            {"a", {{"resource", "scene"}, {"access", "sampled"}, {"subresource", {{"mip", 0}}}}},
            {"b", {{"resource", "scene"}, {"access", "sampled"}, {"subresource", {{"mip", 1}}}}}}; }, field_code,
            {{"field", "resource_ports"}, {"reason", "duplicate_resource"}}},
        {"input settings not array", [](Json &p) { p["input_settings"] = Json::object(); }, field_code,
            {{"field", "input_settings"}, {"reason", "not_an_array"}}},
        {"too many input settings", [](Json &p) { p["input_settings"] = Json::array({nullptr, nullptr}); }, field_code,
            {{"field", "input_settings"}, {"reason", "too_many_entries"}}},
        {"clear arity", [](Json &p) { p["clear_color"] = Json::array({0, 1, 2}); }, field_code,
            {{"field", "clear_color"}, {"reason", "requires_four_numbers"}}},
        {"clear component type", [](Json &p) { p["clear_color"] = Json::array({0, 0, "zero", 1}); }, field_code,
            {{"field", "clear_color"}, {"reason", "requires_four_numbers"}}},
        {"clear overrides not object", [](Json &p) { p["clear_colors"] = Json::array(); }, field_code,
            {{"field", "clear_colors"}, {"reason", "not_an_object"}}},
        {"clear target absent", [](Json &p) { p["clear_colors"] = {{"unknown_color", Json::array({0, 0, 0, 1})}}; }, field_code,
            {{"field", "clear_colors"}, {"reason", "not_a_color_output"}, {"resource", "unknown_color"}}},
        {"implementation object", [](Json &p) { p["implementation"] = "fixture"; }, field_code,
            {{"field", "implementation"}, {"reason", "not_an_object"}}},
        {"implementation extra field", [](Json &p) { p["implementation"] = {{"provider", "fixture"}, {"typo", true}}; }, field_code,
            {{"field", "implementation.typo"}, {"reason", "unknown_field"}}},
        {"implementation empty", [](Json &p) { p["implementation"] = {{"provider", ""}}; }, field_code,
            {{"field", "implementation.provider"}, {"reason", "empty_or_too_long"}}},
        {"implementation length", [](Json &p) { p["implementation"] = {{"provider", std::string(maximumPassImplementationProviderNameBytes + 1, 'a')}}; }, field_code,
            {{"field", "implementation.provider"}, {"reason", "empty_or_too_long"}}},
    };
    for (const auto &entry : refusals) {
        DYNAMIC_SECTION(entry.label) {
            auto invalid = base;
            entry.edit(invalid["rendering_passes"][0]["passes"][1]);
            auto detail = entry.fields;
            detail["graph"] = "main";
            detail["node"] = "tone";
            TestSupport::requireNamedRefusal([&] { (void)compiled(invalid); }, entry.code, detail);
            // The neighboring accepted declaration reaches common completion.
            const auto accepted = compiled(base);
            const auto &ir = *variantOf(accepted, RenderPipelineGraphVariant::flat).render_graph;
            const auto &info = std::get<FullscreenNodeInfo>(ir.graphs.front().nodes.at(1).info);
            CHECK(info.fragment_shader == "engine://scene_present");
        }
    }
    SECTION("reserved define diagnostics retain all structured fields at compiler entry") {
        auto rendering = base;
        auto &defines = rendering["rendering_passes"][0]["passes"][1]["shader"]["defines"];
        defines = Json::array({"LOCAL=1", "PELICAN_PASS_TEXTURE_INPUT_COUNT=5"});
        try { (void)compiled(rendering); FAIL("reserved define accepted"); }
        catch (const PassShaderDefineError &error) {
            CHECK(error.info().pass == "tone");
            CHECK(error.info().define == "PELICAN_PASS_TEXTURE_INPUT_COUNT=5");
            CHECK(error.info().name == "PELICAN_PASS_TEXTURE_INPUT_COUNT");
            CHECK(error.info().index == 1);
            CHECK(error.info().reason == passShaderDefineReserved);
        }
        defines = Json::array({"LOCAL=1", "LOCAL_COUNT=5"});
        CHECK(compiled(rendering).variants.size() == 1);
    }
    SECTION("auxiliary omitted empty vertex pair and skinned forms") {
        for (const auto *type : {"shadow_depth", "velocity", "picking", "debug_draw", "gizmo", "debug_text"}) {
            const bool shadow = std::string_view{type} == "shadow_depth";
            const bool defaults = shadow || std::string_view{type} == "velocity" || std::string_view{type} == "picking";
            Json pass{{"name", "aux"}, {"type", type},
                {"output", {{"color", shadow ? Json(nullptr) : Json("color")}, {"depth", defaults ? Json("depth") : Json(nullptr)}}}};
            Json present = fullscreen("present", shadow ? std::vector<std::string>{} : std::vector<std::string>{"color"}, "swapchain");
            const auto rendering = [&](const Json &aux) {
                return Json{{"render_targets", Json::array({image("color"),
                    Json{{"name", "depth"}, {"extent_scale", 1.0}, {"format", "D32_SFLOAT"},
                         {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}}})},
                    {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", Json::array({fullscreen("initialize", {}, "color"), aux, present})}}})}};
            };
            for (const auto *form : {"omitted", "empty", "vertex", "pair", "skinned"}) {
                INFO(type << " " << form);
                auto authored = pass;
                if (std::string_view{form} != "omitted") authored["shader"] = Json::object();
                if (std::string_view{form} == "vertex" || std::string_view{form} == "pair" || std::string_view{form} == "skinned") {
                    authored["shader"]["vertex"] = "engine://custom_aux";
                }
                if (std::string_view{form} == "pair" || std::string_view{form} == "skinned") authored["shader"]["fragment"] = "engine://custom_aux";
                if (std::string_view{form} == "skinned") authored["shader"]["skinned_vertex"] = "engine://custom_skin";
                const bool accepted = (defaults && std::string_view{form} == "omitted") ||
                    (shadow && std::string_view{form} == "vertex") ||
                    (!shadow && (std::string_view{form} == "pair" || std::string_view{form} == "skinned"));
                if (accepted) {
                    CHECK(compiled(rendering(authored)).variants.size() == 1);
                } else {
                    const bool unsupported = shadow && (std::string_view{form} == "pair" || std::string_view{form} == "skinned");
                    const auto field = unsupported || std::string_view{form} == "vertex" ? "shader.fragment" : "shader.vertex";
                    TestSupport::requireNamedRefusal([&] { (void)compiled(rendering(authored)); },
                        unsupported ? field_code : execution_code,
                        {{"graph", "main"}, {"node", "aux"}, {"field", field},
                         {"reason", unsupported ? "unsupported_stage" : "required"}});
                }
            }
        }
    }
}


TEST_CASE("RC-3c2b2 named material feedback is rejected at compiler entry with a history control",
          "[rc3c2b2][render-compiler][passes][d2]") {
    const auto compiled = [](const Json &rendering) {
        Project project;
        Providers providers;
        return compile(project, providers, rendering, Variants::flat);
    };
    auto rendering = section(0, false);
    rendering["render_targets"].push_back(Json{{"name", "depth"}, {"extent_scale", 1.0},
        {"format", "D32_SFLOAT"}, {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})}});
    for (auto &image : rendering["render_targets"]) if (image["name"] == "post") image["history"] = true;
    auto &pass = rendering["rendering_passes"][0]["passes"][1];
    pass = Json{{"name", "tone"}, {"type", "material"}, {"material_contract", "forward_transparent_v1"},
        {"screen_inputs", {{"opaque_color", "scene"}}},
        {"output", {{"color", "post"}, {"depth", "depth"}}}};
    REQUIRE(compiled(rendering).variants.size() == 1);
    pass["screen_inputs"]["opaque_color"] = "post";
    TestSupport::requireNamedRefusal([&] { (void)compiled(rendering); }, "render_graph.pass_shape@1",
        {{"graph", "main"}, {"node", "tone"}, {"target", "post"}, {"violation", "current_frame_color_feedback"}});
    pass.erase("screen_inputs");
    pass["material_resources"] = {
        {"a", {{"resource", "post"}, {"access", "sampled"}, {"subresource", {{"mip", 0}}}}},
        {"b", {{"resource", "post"}, {"access", "sampled"}, {"subresource", {{"mip", 0}}}}}};
    TestSupport::requireNamedRefusal([&] { (void)compiled(rendering); }, "render_graph.pass_shape@1",
        {{"graph", "main"}, {"node", "tone"}, {"target", "post"}, {"violation", "current_frame_color_feedback"}});
    // The compiler's history control has one binding. The WP326 helper
    // test also checks two bindings to one image and their deduplicated
    // input list; logical shadow planning separately forbids duplicate
    // explicit access declarations for the same resource.
    pass["material_resources"] = {{"a", "post@history"}};
    const auto history = compiled(rendering);
    const auto &variant = variantOf(history, RenderPipelineGraphVariant::flat);
    const auto names = generationRenderTargetNames(history.generation_resources);
    const auto definitions = renderingPassDefinitionsOf(*variant.render_graph, history.generation_resources, names);
    const auto &passes = definitions.at(0).passes;
    const auto found = std::find_if(passes.begin(), passes.end(), [](const auto &p) { return p.name == "tone"; });
    REQUIRE(found != passes.end());
    const auto post_id = names.resolve("post");
    REQUIRE(isConcreteRenderTarget(post_id));
    CHECK(found->input_targets == std::vector<GlobalRenderTargetId>{post_id});
    CHECK(found->input_target_history == std::vector<bool>{true});
    REQUIRE(found->materialInfo().material_resources.size() == 1);
    for (const auto &resource : found->materialInfo().material_resources) {
        CHECK(resource.target == post_id);
        CHECK(resource.history);
    }
    // A current-frame read of an upstream image is accepted; the same
    // binding to this pass's depth attachment is rejected.
    pass["material_resources"] = {{"a", "scene"}};
    CHECK(compiled(rendering).variants.size() == 1);
    pass["material_resources"] = {{"a", "depth"}};
    TestSupport::requireNamedRefusal([&] { (void)compiled(rendering); }, "render_graph.pass_shape@1",
        {{"graph", "main"}, {"node", "tone"}, {"target", "depth"}, {"violation", "current_frame_depth_feedback"}});
}

TEST_CASE("RC-3c2b2 D2 compiler entry retains material filters ranges and variants",
          "[rc3c2b2][render-compiler][passes][d2]") {
    const auto compiled = [](const Json &rendering) {
        Project project;
        Providers providers;
        return compile(project, providers, rendering, Variants::flat);
    };
    auto base = section(0, false);
    base["render_targets"].push_back(Json{{"name", "depth"}, {"extent_scale", 1.0}, {"format", "D32_SFLOAT"},
        {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}});
    base["rendering_passes"][0]["passes"][1] = Json{{"name", "tone"}, {"type", "material"},
        {"material_contract", "forward_opaque_v1"}, {"material_variant", "silhouette"}, {"material_filter", {{"include", {"outlined", "character"}}, {"exclude", {"hidden"}}}},
        {"material_range", {{"start", 3}, {"count", 7}}}, {"output", {{"color", "post"}, {"depth", "depth"}}}};
    struct Refusal { std::string label; std::function<void(Json &)> edit; Json fields; std::string detail; };
    std::vector<Refusal> refusals{
        {"filter object", [](Json &p) { p["material_filter"] = 1; }, {{"field", "material_filter"}, {"reason", "not_an_object"}}, {}},
        {"filter key", [](Json &p) { p["material_filter"] = {{"any", {"outlined"}}}; }, {{"field", "material_filter"}, {"reason", "unknown_field"}, {"key", "any"}}, {}},
        {"filter array", [](Json &p) { p["material_filter"]["include"] = "outlined"; }, {{"field", "material_filter"}, {"reason", "not_a_string_array"}}, {}},
        {"filter tag type", [](Json &p) { p["material_filter"]["include"] = {1}; }, {{"field", "material_filter"}, {"reason", "not_a_string_array"}}, {}},
        {"empty tag", [](Json &p) { p["material_filter"]["include"] = {""}; }, {{"field", "material_filter"}, {"reason", "invalid"}}, "tag is empty or too long"},
        {"duplicate tag", [](Json &p) { p["material_filter"]["include"] = {"outlined", "outlined"}; }, {{"field", "material_filter"}, {"reason", "invalid"}}, "duplicate tag: outlined"},
        {"include exclude overlap", [](Json &p) { p["material_filter"]["exclude"] = {"outlined"}; }, {{"field", "material_filter"}, {"reason", "invalid"}}, "includes and excludes the same tag"},
        {"range object", [](Json &p) { p["material_range"] = 1; }, {{"field", "material_range"}, {"reason", "not_an_object"}}, {}},
        {"range key", [](Json &p) { p["material_range"]["typo"] = 1; }, {{"field", "material_range.typo"}, {"reason", "unknown_field"}}, {}},
        {"unknown contract", [](Json &p) { p["material_contract"] = "unknown_v1"; }, {{"field", "material_contract"}, {"reason", "unknown"}}, {}},
        {"variant contract", [](Json &p) { p["material_variant"] = "silhouette"; p.erase("material_contract"); }, {{"field", "material_variant"}, {"reason", "requires_material_contract"}}, {}},
        {"variant filter", [](Json &p) { p["material_variant"] = "silhouette"; p["material_filter"]["include"] = Json::array(); }, {{"field", "material_variant"}, {"reason", "requires_material_filter_include"}}, {}},
        {"variant name", [](Json &p) { p["material_variant"] = "bad/name"; }, {{"field", "material_variant"}, {"reason", "invalid_name"}}, {}},
        {"states schema", [](Json &p) { p["material_output_states"] = Json::object(); }, {{"field", "material_output_states"}, {"reason", "requires_material_outputs"}}, {}},
    };
    for (const auto *field : {"start", "count"}) {
        const std::string path = "material_range." + std::string{field};
        for (const auto &value : std::vector<Json>{-1, 1.5, "3", nullptr, 4294967296ULL}) {
            const std::string reason = value.is_number_unsigned() ? "too_large" : "requires_unsigned_integer";
            refusals.push_back({path + "=" + value.dump(), [field, value](Json &p) { p["material_range"][field] = value; },
                {{"field", path}, {"reason", reason}}, {}});
        }
        refusals.push_back({path + " missing", [field](Json &p) { p["material_range"].erase(field); },
            {{"field", path}, {"reason", "requires_unsigned_integer"}}, {}});
    }
    for (const auto &entry : refusals) {
        DYNAMIC_SECTION(entry.label) {
            auto invalid = base;
            entry.edit(invalid["rendering_passes"][0]["passes"][1]);
            auto fields = entry.fields;
            fields["graph"] = "main"; fields["node"] = "tone";
            TestSupport::requireNamedRefusal([&] { (void)compiled(invalid); }, "render_graph.node_field_invalid@1", fields);
            if (!entry.detail.empty()) {
                try { (void)compiled(invalid); FAIL("invalid material accepted"); }
                catch (const GraphRegistryError &error) {
                    CHECK(error.detail().at("detail").get<std::string>().find(entry.detail) != std::string::npos);
                }
            }
            const auto accepted = compiled(base);
            const auto &variant = variantOf(accepted, RenderPipelineGraphVariant::flat);
            const auto names = generationRenderTargetNames(accepted.generation_resources);
            const auto definitions = renderingPassDefinitionsOf(*variant.render_graph, accepted.generation_resources, names);
            const auto &info = definitions.at(0).passes.at(1).materialInfo();
            CHECK(info.material_variant == std::optional<std::string>{"silhouette"});
            REQUIRE(info.material_filter);
            CHECK(info.material_filter->include == std::vector<std::string>{"character", "outlined"});
            CHECK(info.material_filter->exclude == std::vector<std::string>{"hidden"});
            CHECK(info.material_start == 3);
            CHECK(info.material_count == 7);
        }
    }
}

} // namespace Pelican
