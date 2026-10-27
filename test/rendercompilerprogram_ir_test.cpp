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
            pass["after"] = Json::array({"initialize"});
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

namespace {
struct PassCompilerRefusal {
    std::string label;
    std::function<void(Json &)> edit;
    std::string code;
    Json fields;
    bool provider_revision = false;
};

RenderCompilerProgramResult compilePassFixture(const Json &rendering, const RenderCompilerProgram *program = nullptr,
                                             std::function<void(Json &)> compose_runtime_config = {}) {
    Project project;
    Providers providers;
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).headless = false;
    auto requests = requestsOf(Variants::flat);
    requests.front().compose_runtime_config = std::move(compose_runtime_config);
    const auto transforms = providers.transforms.snapshot();
    const auto strategies = providers.strategies.snapshot();
    const auto subgraphs = providers.subgraphs.snapshot();
    const VulkanRenderCompilerBackendContext backend{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    // These empty graph roots contain no value/pose nodes to lower. Feed
    // the declaration into the program itself, so malformed short forms
    // must be refused inside this entry, rather than by fixture preparation.
    return runRenderCompilerProgram(program ? *program : defaultVulkanRenderCompilerProgram(), RenderCompilerProgramInput{
        .rendering_config = project.rootOf(rendering),
        .source_name = "rc3c2b2-d2",
        .path_resolver = project.paths,
        .load_document = [&project](std::string_view ref) { return project.load(ref); },
        .runtime_shader_compiler_enabled = true,
        .graph_transforms = transforms,
        .render_strategies = strategies,
        .subgraph_replacements = subgraphs,
        .backend_context = backend,
        .variants = requests,
    });
}

RenderCompilerProgramResult compilePassRevisionFixture(const Json &rendering, std::function<void(Json &)> edit) {
    Project project;
    Providers providers;
    providers.first.behavior = TransformBehavior::edit_response;
    providers.first.edit_response = std::move(edit);
    auto input = rendering;
    input["graph_transforms"] = Json::array({Json{{"name", "rc3c1b.t1"}, {"provider", "rc3c1b.transform_1"}}});
    return compile(project, providers, input, Variants::flat);
}

TEST_CASE("RC-5s output terminal waits for provider compute and its render consumer", "[rendercompiler][rc5s][review-terminal]") {
    const auto result = compilePassRevisionFixture(section(0, false), [](Json &config) {
        config["buffers"].push_back(Json{{"name", "provider_buffer"}, {"size", 16}});
        config["render_targets"].push_back(image("provider_image"));
        config["compute_tasks"].push_back(Json{{"name", "provider_compute"},
            {"shader", "engine://shaders/compute/fixture"}, {"dispatch", {{"groups", {1, 1, 1}}}},
            {"after", {"present"}}, {"writes", {"provider_buffer"}}});
        auto pass = fullscreen("provider_pass", {}, "provider_image", {"provider_compute"});
        auto &passes = config["rendering_passes"][0]["passes"];
        passes.insert(std::prev(passes.end()), pass);
    });
    const auto &variant = result.variants.at(0);
    const auto order = framePlanOrder(variant.frame_plans.at("main"));
    const auto at = [&](const std::string &name) { return std::find(order.begin(), order.end(), name); };
    CHECK(at("present") < at("provider_compute"));
    CHECK(at("provider_compute") < at("provider_pass"));
    CHECK(at("provider_pass") < at("output_transform"));
    CHECK(order.back() == "output_transform");
}

TEST_CASE("RC-5s graph provider refuses removed authoring at its response boundary", "[rendercompiler][rc5s][review-boundary]") {
    for (const bool insert : {false, true}) {
        try {
            (void)compilePassRevisionFixture(section(0, false), [=](Json &config) {
                config["rendering_passes"][0]["passes"][0][insert ? "insert" : "canonical_anchor"] =
                    insert ? "after:post_ldr" : "post_ldr";
            });
            FAIL("removed provider authoring was accepted");
        } catch (const GraphRegistryError &error) {
            CHECK(error.code() == "render_graph.canonical_anchor_removed@1");
            CHECK(error.detail().at("document") == "provider://rc3c1b.transform_1");
            CHECK(error.detail().at("pointer") == std::string("/rendering_passes/0/passes/0/") +
                (insert ? "insert" : "canonical_anchor"));
        }
    }
}

Json materialCompilerFixture() {
    auto base = section(0, false);
    base["render_targets"].push_back(Json{{"name", "depth"}, {"extent_scale", 1.0}, {"format", "D32_SFLOAT"},
        {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})}});
    base["render_targets"].push_back(Json{{"name", "source_depth"}, {"extent_scale", 1.0}, {"format", "D32_SFLOAT"},
        {"source", {{"type", "solid"}}}, {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})}});
    base["buffers"] = Json::array({Json{{"name", "data"}, {"size", 64}}});
    base["rendering_passes"][0]["passes"][1] = Json{{"name", "tone"}, {"type", "material"},
        {"material_contract", "forward_opaque_v1"}, {"output", {{"color", "post"}, {"depth", "depth"}}}};
    return base;
}

void checkPassCompilerRefusals(const Json &base, const std::vector<PassCompilerRefusal> &rows) {
    REQUIRE(compilePassFixture(base).variants.size() == 1);
    for (const auto &row : rows) {
        DYNAMIC_SECTION(row.label) {
            auto invalid = base;
            row.edit(invalid);
            TestSupport::requireNamedRefusal([&] {
                if (row.provider_revision) (void)compilePassRevisionFixture(base, row.edit);
                else (void)compilePassFixture(invalid);
            }, row.code, row.fields);
            if (row.provider_revision) CHECK(compilePassRevisionFixture(base, {}).variants.size() == 1);
            const auto accepted = compilePassFixture(base);
            const auto &variant = variantOf(accepted, RenderPipelineGraphVariant::flat);
            const auto definitions = renderingPassDefinitionsOf(*variant.render_graph, accepted.generation_resources,
                generationRenderTargetNames(accepted.generation_resources));
            const auto &passes = definitions.at(0).passes;
            CHECK(std::any_of(passes.begin(), passes.end(), [](const auto &pass) { return pass.name == "tone"; }));
        }
    }
}
} // namespace

TEST_CASE("RC-3c2b2 D2 compiler entry retains material named resource contracts",
          "[rc3c2b2][render-compiler][passes][d2]") {
    auto base = materialCompilerFixture();
    // The source depth is produced upstream, independently of the subject's depth.
    base["render_targets"].back().erase("source");
    base["rendering_passes"][0]["passes"].push_back(Json{{"name", "shadow"}, {"type", "shadow_depth"},
        {"output", {{"color", nullptr}, {"depth", "source_depth"}}}});
    const std::string field_code = "render_graph.node_field_invalid@1";
    std::vector<PassCompilerRefusal> rows;
    const auto field = [&](std::string label, std::function<void(Json &)> edit,
                           std::string path, std::string reason, Json more = Json::object()) {
        more["graph"] = "main"; more["node"] = "tone"; more["field"] = path; more["reason"] = reason;
        rows.push_back({std::move(label), [edit](Json &r) { edit(r["rendering_passes"][0]["passes"][1]); }, field_code, more});
    };
    for (const auto *key : {"screen_inputs", "surface_resources", "material_resources"}) {
        field(std::string{key}+" object", [key](Json &p) { p[key] = 4; }, key, "not_an_object");
        field(std::string{key}+" positional conflict", [key](Json &p) { p[key] = Json::object(); p["input"] = Json::array(); }, key, "replaces_positional_input");
    }
    field("screen entry type", [](Json &p) { p["screen_inputs"] = {{"opaque_color", 2}}; }, "screen_inputs", "entry_not_a_resource", {{"key", "opaque_color"}});
    field("unknown screen contract", [](Json &p) { p["screen_inputs"] = {{"history_magic", "scene"}}; }, "screen_inputs", "unknown_contract");
    field("surface is feature owned", [](Json &p) { p["surface_resources"] = {{"opaque_depth", "source_depth"}}; }, "surface_resource", "not_feature_owned", {{"port", "opaque_depth"}});
    field("buffer history", [](Json &p) { p["material_resources"] = {{"previous", "data@history"}}; }, "material_resources.previous", "buffer_history");
    field("buffer sampled", [](Json &p) { p["material_resources"] = {{"data", {{"resource", "data"}, {"access", "sampled"}}}}; }, "material_resources.data", "requires_storage");
    field("buffer view", [](Json &p) { p["material_resources"] = {{"data", {{"resource", "data"}, {"view", "per_view"}}}}; }, "material_resources.data", "buffer_image_view");
    field("explicit default buffer sampling", [](Json &p) { p["material_resources"] = {{"data", {{"resource", "data"}, {"sampling", Json::object()}}}}; }, "material_resources.data", "buffer_sampling");
    field("image storage", [](Json &p) { p["material_resources"] = {{"color", {{"resource", "scene"}, {"access", "storage"}}}}; }, "material_resources.color", "requires_sampled");
    field("material positional input", [](Json &p) { p["input"] = Json::array({"scene"}); }, "input", "requires_named_material_inputs");
    field("resource entry", [](Json &p) { p["material_resources"] = {{"color", 1}}; }, "material_resources", "entry_not_a_resource", {{"key", "color"}});
    field("resource string", [](Json &p) { p["material_resources"] = {{"color", {{"resource", 1}}}}; }, "material_resources", "resource_not_a_string", {{"key", "color"}});
    field("resource empty", [](Json &p) { p["screen_inputs"] = {{"opaque_color", ""}}; }, "screen_input", "empty_name");
    const auto special = [&](std::string label, std::function<void(Json &)> edit, std::string code, Json fields) {
        fields["graph"] = "main"; fields["node"] = "tone";
        rows.push_back({std::move(label), [edit](Json &r) { edit(r["rendering_passes"][0]["passes"][1]); }, std::move(code), fields});
    };
    special("screen source type", [](Json &p) { p["screen_inputs"] = {{"opaque_color", "source_depth"}}; },
        "render_graph.material_input_invalid@1", {{"resource", "source_depth"}, {"port", "opaque_color"}});
    special("surface source type", [](Json &p) { p["surface_resources"] = {{"directional_shadow", "scene"}}; },
        "render_graph.material_input_invalid@1", {{"resource", "scene"}, {"port", "directional_shadow"}});
    special("resource unknown", [](Json &p) { p["material_resources"] = {{"color", "ghost"}}; },
        "render_graph.unknown_resource@1", {{"resource", "ghost"}});
    special("same pixel selected mip", [](Json &p) { p["material_resources"] = {{"color", {{"resource", "scene"}, {"subresource", {{"mip", 0}}}, {"footprint", "same_pixel"}}}}; },
        "render_graph.footprint_invalid@1", {{"field", "material_resources"}, {"resource", "color"}, {"reason", "same_pixel_subresource"}});
    special("same pixel cube", [](Json &p) { p["material_resources"] = {{"color", {{"resource", "scene"}, {"view", "cube"}, {"footprint", "same_pixel"}}}}; },
        "render_graph.footprint_invalid@1", {{"field", "material_resources"}, {"resource", "color"}, {"reason", "same_pixel_cube_view"}});
    special("material resource mixes current and history", [](Json &p) {
        p["material_resources"] = {{"current", "scene"}, {"previous", "scene@history"}};
    }, "render_graph.pass_shape@1", {{"violation", "duplicate_input"}, {"target", "scene"}});
    checkPassCompilerRefusals(base, rows);

    SECTION("accepted named inputs retain their resolved contracts") {
        auto r = base;
        r["rendering_passes"][0]["passes"][1]["screen_inputs"] = {{"opaque_color", "scene"}, {"linear_view_depth", "source_depth"}};
        const auto accepted = compilePassFixture(r);
        const auto definitions = renderingPassDefinitionsOf(*variantOf(accepted, RenderPipelineGraphVariant::flat).render_graph,
            accepted.generation_resources, generationRenderTargetNames(accepted.generation_resources));
        const auto &info = definitions.at(0).passes.at(1).materialInfo();
        REQUIRE(info.screen_inputs.size() == 2);
        const auto &linear = info.screen_inputs.at(0).contract;
        CHECK(linear.name == "linear_view_depth");
        CHECK(linear.conversion == std::optional<std::string>{"pelican.render.depth_linearize@1"});
        CHECK(linear.footprint.kind == LogicalReadFootprintKind::same_pixel);
        r["rendering_passes"][0]["passes"][1].erase("screen_inputs");
        r["rendering_passes"][0]["passes"][1]["surface_resources"] = {{"directional_shadow", "source_depth"}};
        CHECK(compilePassFixture(r).variants.size() == 1);
        r["rendering_passes"][0]["passes"][1].erase("surface_resources");
        r["rendering_passes"][0]["passes"][1]["material_resources"] = {{"positions", "data"}, {"color", "scene"}};
        CHECK(compilePassFixture(r).variants.size() == 1);
        r["render_targets"][0]["history"] = true;
        r["rendering_passes"][0]["passes"][1]["material_resources"] = {{"previous", "scene@history"}};
        CHECK(compilePassFixture(r).variants.size() == 1);
    }
}

TEST_CASE("RC-3c2b2 D2 compiler entry retains raster execution declarations",
          "[rc3c2b2][render-compiler][passes][d2]") {
    auto base = section(0, false);
    auto &tone = base["rendering_passes"][0]["passes"][1];
    tone["type"] = "raster";
    tone["draw"] = {{"implementation", "project.fixture_draw@1"}, {"vertex_count", 3}};
    tone["shader"]["implementation"] = "project.fixture_shader@1";
    std::vector<PassCompilerRefusal> rows;
    const auto field = [&](std::string label, std::function<void(Json &)> edit, std::string path,
                           std::string reason = "invalid", std::string code = "render_graph.node_field_invalid@1") {
        rows.push_back({std::move(label), [edit](Json &r) { edit(r["rendering_passes"][0]["passes"][1]); }, std::move(code),
            {{"graph", "main"}, {"node", "tone"}, {"field", path}, {"reason", reason}}});
    };
    field("missing raster draw", [](Json &p) { p.erase("draw"); }, "draw", "required", "render_graph.pass_execution_invalid@1");
    for (const auto *stage : {"vertex", "fragment"}) {
        field(std::string{"missing raster "}+stage, [stage](Json &p) { p["shader"].erase(stage); }, std::string{"shader."}+stage, "required", "render_graph.pass_execution_invalid@1");
        field(std::string{"raster stage type "}+stage, [stage](Json &p) { p["shader"][stage] = 4; }, std::string{"shader."}+stage, "requires_string");
    }
    field("raster shader object", [](Json &p) { p["shader"] = 1; }, "shader", "not_an_object");
    field("raster shader key", [](Json &p) { p["shader"]["typo"] = "ignored"; }, "shader.typo", "unknown_field");
    field("raster implementation id", [](Json &p) { p["shader"]["implementation"] = "project.shader"; }, "shader.implementation", "not_a_canonical_id");
    field("raster implementation type", [](Json &p) { p["shader"]["implementation"] = 2; }, "shader.implementation", "requires_string");
    field("raster draw object", [](Json &p) { p["draw"] = 1; }, "draw/raster_state/clip_y");
    field("raster draw key", [](Json &p) { p["draw"]["typo"] = 1; }, "draw/raster_state/clip_y");
    field("raster draw count", [](Json &p) { p["draw"]["vertex_count"] = -1; }, "draw/raster_state/clip_y");
    field("raster draw operation", [](Json &p) { p["draw"]["operation"] = "mystery"; }, "draw/raster_state/clip_y");
    field("raster clip direction", [](Json &p) { p["clip_y"] = "sideways"; }, "draw/raster_state/clip_y");
    field("raster state key", [](Json &p) { p["raster_state"] = {{"typo", true}}; }, "draw/raster_state/clip_y");
    field("raster MRT state count", [](Json &p) { p["raster_state"] = {{"color_attachments", Json::array()}}; }, "draw/raster_state/clip_y");
    field("raster depth without depth", [](Json &p) { p["raster_state"] = {{"depth_write", true}}; }, "draw/raster_state/clip_y");
    field("raster port input edge", [](Json &p) { p["resource_ports"] = {{"color", "ghost"}}; }, "resource_ports");
    field("raster port sampled access", [](Json &p) { p["resource_ports"] = {{"color", {{"resource", "scene"}, {"access", "storage"}}}}; }, "resource_ports", "invalid_access");
    field("raster port duplicate", [](Json &p) { p["resource_ports"] = {{"a", "scene"}, {"b", "scene"}}; }, "resource_ports", "invalid");
    checkPassCompilerRefusals(base, rows);
}

TEST_CASE("RC-3c2b2 D2 compiler entry retains GPU draw source shape and buffer contracts",
          "[rc3c2b2][render-compiler][passes][d2]") {
    auto base = materialCompilerFixture();
    base["render_targets"].erase(base["render_targets"].end()-1);
    base["buffers"] = Json::array({Json{{"name", "commands"}, {"size", 320}, {"command_layout", "indexed_draw"}},
        Json{{"name", "count"}, {"size", 4}, {"command_layout", "draw_count"}},
        Json{{"name", "segments"}, {"size", 128}, {"host_source", "scene_draw_segments_v1"}}});
    auto &tone = base["rendering_passes"][0]["passes"][1];
    tone["material_range"] = {{"start", 0}, {"count", 1}};
    tone["gpu_draw_source"] = {{"commands", "commands"}, {"count", "count"}, {"max_draw_count", 16}};
    std::vector<PassCompilerRefusal> rows;
    const auto field = [&](std::string label, std::function<void(Json &)> edit, std::string path, std::string reason) {
        rows.push_back({std::move(label), [edit](Json &r) { edit(r["rendering_passes"][0]["passes"][1]); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "tone"}, {"field", path}, {"reason", reason}}});
    };
    field("GPU draw object", [](Json &p) { p["gpu_draw_source"] = 1; }, "gpu_draw_source", "not_an_object");
    field("GPU draw key", [](Json &p) { p["gpu_draw_source"]["typo"] = 1; }, "gpu_draw_source", "unknown_field");
    for (const auto *key : {"commands", "count"}) {
        field(std::string{"GPU missing "}+key, [key](Json &p) { p["gpu_draw_source"].erase(key); }, std::string{"gpu_draw_source."}+key, "requires_string");
        field(std::string{"GPU non-string "}+key, [key](Json &p) { p["gpu_draw_source"][key] = 1; }, std::string{"gpu_draw_source."}+key, "requires_string");
        field(std::string{"GPU empty "}+key, [key](Json &p) { p["gpu_draw_source"][key] = ""; }, "gpu_draw_source", "empty_buffer_name");
    }
    for (const auto *key : {"max_draw_count", "command_offset", "count_offset"}) {
        field(std::string{"GPU negative "}+key, [key](Json &p) { p["gpu_draw_source"][key] = -1; }, std::string{"gpu_draw_source."}+key, "requires_unsigned_integer");
    }
    for (const auto *key : {"command_offset", "count_offset"}) {
        field(std::string{"GPU alignment "}+key, [key](Json &p) { p["gpu_draw_source"][key] = 2; }, "gpu_draw_source", "offset_unaligned");
    }
    field("GPU count overflow", [](Json &p) { p["gpu_draw_source"]["max_draw_count"] = 4294967296ULL; }, "gpu_draw_source.max_draw_count", "too_large");
    field("GPU count zero", [](Json &p) { p["gpu_draw_source"]["max_draw_count"] = 0; }, "gpu_draw_source", "max_draw_count_zero");
    field("GPU equal buffers", [](Json &p) { p["gpu_draw_source"]["count"] = "commands"; }, "gpu_draw_source", "commands_equal_count");
    field("GPU layout", [](Json &p) { p["gpu_draw_source"]["layout"] = "mystery"; }, "gpu_draw_source", "unknown_layout");
    field("GPU fixed segments", [](Json &p) { p["gpu_draw_source"]["segments"] = "segments"; }, "gpu_draw_source", "fixed_state_with_segments");
    field("GPU missing segments", [](Json &p) { p["gpu_draw_source"]["layout"] = "draw_queue_segments_v1"; }, "gpu_draw_source", "segments_required");
    field("GPU aliased segments", [](Json &p) { p["gpu_draw_source"]["layout"] = "draw_queue_segments_v1"; p["gpu_draw_source"]["segments"] = "count"; }, "gpu_draw_source", "buffer_names_not_distinct");
    field("GPU fallback", [](Json &p) { p["gpu_draw_source"]["fallback"] = "skip"; }, "gpu_draw_source", "unknown_fallback");
    field("GPU execution", [](Json &p) { p["gpu_draw_source"]["execution"] = "gpu"; }, "gpu_draw_source", "unknown_execution");
    field("GPU fixed material range", [](Json &p) { p["material_range"]["count"] = 2; }, "gpu_draw_source", "fixed_state_requires_one_material");
    field("GPU segmented material range", [](Json &p) { p["material_range"]["count"] = 0; p["gpu_draw_source"]["layout"] = "draw_queue_segments_v1"; p["gpu_draw_source"]["segments"] = "segments"; }, "gpu_draw_source", "segments_require_material_range");
    for (const auto &[index, role, expected] : std::vector<std::tuple<int, std::string, std::string>>{{0, "commands", "indexed_draw"}, {1, "count", "draw_count"}}) {
        rows.push_back({"GPU buffer layout " + role, [index](Json &r) { r["buffers"][index].erase("command_layout"); },
            "render_graph.gpu_draw_source_invalid@1", {{"graph", "main"}, {"node", "tone"}, {"field", "gpu_draw_source." + role}, {"reason", "command_layout_mismatch"}, {"buffer", role}, {"expected", expected}}});
    }
    for (const auto &[key, value, reason] : std::vector<std::tuple<std::string, int, std::string>>{{"command_offset", 4, "command_range_exceeds_buffer"}, {"count_offset", 4, "count_exceeds_buffer"}}) {
        rows.push_back({"GPU buffer capacity " + key, [key, value](Json &r) { r["rendering_passes"][0]["passes"][1]["gpu_draw_source"][key] = value; },
            "render_graph.gpu_draw_source_invalid@1", {{"graph", "main"}, {"node", "tone"}, {"reason", reason}}});
    }
    rows.push_back({"GPU draw is material only", [](Json &r) {
        auto &p = r["rendering_passes"][0]["passes"][1];
        p["type"] = "fullscreen";
        p.erase("material_contract"); p.erase("material_range");
    }, "render_graph.pass_field_ownership@1", {{"graph", "main"}, {"node", "tone"}}, true});
    checkPassCompilerRefusals(base, rows);
    SECTION("returned IR offsets are aligned independently of JSON parsing") {
        auto rendering = base;
        rendering["buffers"][0]["size"] = 64;
        rendering["buffers"][1]["size"] = 8;
        rendering["rendering_passes"][0]["passes"][1]["gpu_draw_source"]["max_draw_count"] = 1;
        for (const bool command : {true, false}) {
            INFO((command ? "command_offset" : "count_offset"));
            const auto compiled = [&](std::uint64_t offset) {
                EditingProgram program;
                program.edit = [command, offset](RenderGraphIR &ir) {
                    auto &source = *std::get<MaterialNodeInfo>(ir.graphs.at(0).nodes.at(1).info).gpu_draw_source;
                    (command ? source.command_offset : source.count_offset) = offset;
                };
                return compilePassFixture(rendering, &program);
            };
            const auto accepted = compiled(0);
            const auto definitions = renderingPassDefinitionsOf(*variantOf(accepted, RenderPipelineGraphVariant::flat).render_graph,
                accepted.generation_resources, generationRenderTargetNames(accepted.generation_resources));
            const auto &source = definitions.at(0).passes.at(1).materialInfo().gpu_draw_source;
            REQUIRE(source.has_value());
            CHECK(source->command_offset == 0);
            CHECK(source->count_offset == 0);
            TestSupport::requireNamedRefusal([&] { (void)compiled(2); }, "render_graph.gpu_draw_source_invalid@1",
                {{"graph", "main"}, {"node", "tone"},
                 {"field", command ? "gpu_draw_source.command_offset" : "gpu_draw_source.count_offset"},
                 {"reason", "not_4_byte_aligned"}, {"offset", 2}});
        }
    }
    auto segmented = base;
    segmented["rendering_passes"][0]["passes"][1]["material_range"]["count"] = 2;
    segmented["rendering_passes"][0]["passes"][1]["gpu_draw_source"]["layout"] = "draw_queue_segments_v1";
    segmented["rendering_passes"][0]["passes"][1]["gpu_draw_source"]["segments"] = "segments";
    CHECK(compilePassFixture(segmented).variants.size() == 1);
    auto wrong = segmented;
    wrong["buffers"][2].erase("host_source");
    TestSupport::requireNamedRefusal([&] { (void)compilePassFixture(wrong); }, "render_graph.gpu_draw_source_invalid@1",
        {{"graph", "main"}, {"node", "tone"}, {"buffer", "segments"}, {"reason", "host_source_mismatch"}, {"expected", "scene_draw_segments_v1"}});
}

TEST_CASE("RC-3c2b2 D2 compiler entry retains material schemas and sparse attachment states",
          "[rc3c2b2][render-compiler][passes][d2]") {
    auto base = materialCompilerFixture();
    base["render_targets"].erase(base["render_targets"].end()-1);
    auto &tone = base["rendering_passes"][0]["passes"][1];
    tone["material_contract"] = "forward_opaque_v1";
    tone["material_outputs"] = {{"schema", "pelican.material_outputs"}, {"version", 1}, {"name", "project.fixture"},
        {"outputs", Json::array({Json{{"name", "color"}, {"type", "vec4"}, {"source", "surface.base_color"}}})}};
    tone["material_output_states"] = {{"color", {{"blend", "additive"}, {"write_mask", "rg"}}}};
    std::vector<PassCompilerRefusal> rows;
    const auto field = [&](std::string label, std::function<void(Json &)> edit, const std::string &path) {
        rows.push_back({std::move(label), [edit](Json &r) { edit(r["rendering_passes"][0]["passes"][1]); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "tone"}, {"field", path}, {"reason", "invalid"}}});
    };
    field("schema object", [](Json &p) { p["material_outputs"] = 4; }, "material_outputs");
    field("schema tag", [](Json &p) { p["material_outputs"]["schema"] = "unknown"; }, "material_outputs");
    field("schema version", [](Json &p) { p["material_outputs"]["version"] = 2; }, "material_outputs");
    field("schema key", [](Json &p) { p["material_outputs"]["typo"] = true; }, "material_outputs");
    field("schema empty name", [](Json &p) { p["material_outputs"]["name"] = ""; }, "material_outputs");
    field("schema outputs array", [](Json &p) { p["material_outputs"]["outputs"] = 1; }, "material_outputs");
    field("schema empty outputs", [](Json &p) { p["material_outputs"]["outputs"] = Json::array(); }, "material_outputs");
    field("schema output object", [](Json &p) { p["material_outputs"]["outputs"][0] = 1; }, "material_outputs");
    field("schema output key", [](Json &p) { p["material_outputs"]["outputs"][0]["typo"] = true; }, "material_outputs");
    field("schema output identifier", [](Json &p) { p["material_outputs"]["outputs"][0]["name"] = "bad/name"; }, "material_outputs");
    field("schema output type", [](Json &p) { p["material_outputs"]["outputs"][0]["type"] = "mystery"; }, "material_outputs");
    field("schema output source", [](Json &p) { p["material_outputs"]["outputs"][0]["source"] = "mystery"; }, "material_outputs");
    field("schema duplicate output", [](Json &p) { p["material_outputs"]["outputs"].push_back(p["material_outputs"]["outputs"][0]); }, "material_outputs");
    field("schema nonfloating source", [](Json &p) { p["material_outputs"]["outputs"][0]["type"] = "uint"; }, "material_outputs");
    field("states object", [](Json &p) { p["material_output_states"] = 1; }, "material_output_states");
    field("state object", [](Json &p) { p["material_output_states"]["color"] = 1; }, "material_output_states");
    field("state key", [](Json &p) { p["material_output_states"]["color"]["typo"] = 1; }, "material_output_states");
    field("state unknown output", [](Json &p) { p["material_output_states"]["unknown"] = {{"write_mask", "rgba"}}; }, "material_output_states");
    field("state empty override", [](Json &p) { p["material_output_states"]["color"] = Json::object(); }, "material_output_states");
    field("state duplicate mask", [](Json &p) { p["material_output_states"]["color"]["write_mask"] = "rr"; }, "material_output_states");
    field("state blend enum", [](Json &p) { p["material_output_states"]["color"]["blend"] = "mystery"; }, "material_output_states");
    field("state integer blend", [](Json &p) { p["material_outputs"]["outputs"][0]["type"] = "uint"; p["material_outputs"]["outputs"][0]["source"] = "custom"; }, "material_output_states");
    const auto attachment = [&](std::string label, std::function<void(Json &)> edit, std::string reason, Json more = Json::object()) {
        more["graph"] = "main"; more["node"] = "tone"; more["reason"] = reason;
        rows.push_back({std::move(label), [edit](Json &r) { edit(r["rendering_passes"][0]["passes"][1]); },
            "render_graph.material_attachment_invalid@1", more});
    };
    attachment("schema attachment count", [](Json &p) { p["output"]["color"] = Json::array({"post", "scratch"}); }, "color_count", {{"expected", 1}});
    attachment("schema attachment numeric class", [](Json &p) {
        p["material_outputs"]["outputs"][0]["type"] = "uint";
        p["material_outputs"]["outputs"][0]["source"] = "custom";
        p["material_output_states"]["color"]["blend"] = "opaque";
    }, "numeric_class", {{"resource", "post"}, {"output", "color"}});
    attachment("schema deferred lit data", [](Json &p) {
        p["material_contract"] = "deferred_geometry_v1";
        p["material_outputs"]["outputs"][0]["source"] = "lighting.scene_color";
    }, "scene_color_in_deferred");
    attachment("schema requires depth", [](Json &p) { p["output"]["depth"] = nullptr; }, "requires_depth");
    attachment("schema requires explicit contract", [](Json &p) { p["material_contract"] = "legacy_gbuffer_v1"; }, "outputs_require_contract");
    checkPassCompilerRefusals(base, rows);
    const auto accepted = compilePassFixture(base);
    const auto definitions = renderingPassDefinitionsOf(*variantOf(accepted, RenderPipelineGraphVariant::flat).render_graph,
        accepted.generation_resources, generationRenderTargetNames(accepted.generation_resources));
    const auto &info = definitions.at(0).passes.at(1).materialInfo();
    REQUIRE(info.output_schema);
    CHECK(info.output_schema->outputs.size() == 1);
    REQUIRE(info.output_states.size() == 1);
    CHECK(info.output_states.front().output == "color");
    CHECK(info.output_states.front().blend->enabled);
    CHECK(info.output_states.front().write_mask == (materialOutputWriteRed | materialOutputWriteGreen));
}

TEST_CASE("RC-3c2b2 D2 compiler entry retains attachment and input vocabulary",
          "[rc3c2b2][render-compiler][passes][d2]") {
    const auto base = section(0, false);
    std::vector<PassCompilerRefusal> rows;
    const auto add = [&](std::string label, std::function<void(Json &)> edit, std::string code, Json fields) {
        fields["graph"] = "main"; fields["node"] = "tone";
        rows.push_back({std::move(label), [edit](Json &r) { edit(r["rendering_passes"][0]["passes"][1]); }, std::move(code), fields});
    };
    for (const auto *aspect : {"color", "depth"}) {
        const std::string path = "output." + std::string{aspect};
        add(path+" missing", [aspect](Json &p) { p["output"].erase(aspect); }, "render_graph.node_field_invalid@1", {{"field", path}, {"reason", "missing"}});
        add(path+" unknown key", [aspect](Json &p) { p["output"][aspect] = {{"target", "post"}, {"typo", 1}}; }, "render_graph.attachment_invalid@1", {{"field", path}, {"reason", "unknown_field"}, {"key", "typo"}});
        add(path+" missing target", [aspect](Json &p) { p["output"][aspect] = Json::object(); }, "render_graph.attachment_invalid@1", {{"field", path}, {"reason", "requires_target"}});
        add(path+" empty target", [aspect](Json &p) { p["output"][aspect] = {{"target", ""}}; }, "render_graph.attachment_invalid@1", {{"field", path}, {"reason", "target_empty"}});
        add(path+" selected mips", [aspect](Json &p) { p["output"][aspect] = {{"target", "post"}, {"subresource", {{"mip_count", 2}}}}; }, "render_graph.attachment_invalid@1", {{"field", path}, {"reason", "subresource_not_one_mip"}});
        add(path+" swapchain subresource", [aspect](Json &p) { p["output"][aspect] = {{"target", "swapchain"}, {"subresource", {{"mip", 0}}}}; }, "render_graph.attachment_invalid@1", {{"field", path}, {"reason", "swapchain_subresource"}});
        for (const auto *op : {"load_op", "store_op"}) {
            add(path+op+" type", [aspect, op](Json &p) { p["output"][aspect] = {{"target", "post"}, {op, 1}}; }, "render_graph.attachment_invalid@1", {{"field", path}, {"reason", "op_not_a_string"}});
            add(path+op+" unknown", [aspect, op](Json &p) { p["output"][aspect] = {{"target", "post"}, {op, "mystery"}}; }, "render_graph.attachment_invalid@1", {{"field", path}, {"reason", "unknown_op"}});
        }
    }
    add("output object", [](Json &p) { p["output"] = 1; }, "render_graph.node_field_invalid@1", {{"field", "output"}, {"reason", "requires_object"}});
    add("input element type", [](Json &p) { p["input"] = Json::array({1}); }, "render_graph.node_field_invalid@1", {{"field", "input"}, {"reason", "not_a_string_array"}});
    add("input invalid qualifier", [](Json &p) { p["input"] = Json::array({"scene@bad"}); }, "render_graph.resource_reference_invalid@1", {{"field", "input"}, {"resource", "scene@bad"}});
    add("empty input", [](Json &p) { p["input"] = Json::array({""}); }, "render_graph.resource_reference_invalid@1", {{"field", "input"}, {"resource", ""}});
    add("clear override arity", [](Json &p) { p["clear_colors"] = {{"post", Json::array({0, 1})}}; }, "render_graph.node_field_invalid@1", {{"field", "clear_colors.post"}, {"reason", "requires_four_numbers"}});
    for (const auto *key : {"color_load_op", "color_store_op"}) {
        add(std::string{key}+" type", [key](Json &p) { p[key] = 2; }, "render_graph.node_field_invalid@1", {{"field", key}, {"reason", "requires_string"}});
        add(std::string{key}+" enum", [key](Json &p) { p[key] = "mystery"; }, "render_graph.node_field_invalid@1", {{"field", key}, {"reason", "unknown_op"}});
    }
    for (const auto *key : {"depth_load_op", "depth_store_op"}) {
        add(std::string{key}+" requires output", [key](Json &p) { p[key] = "mystery"; }, "render_graph.attachment_option_without_output@1", {{"field", key}});
    }
    for (auto &row : rows) {
        if (row.label == "output.color missing target" || row.label == "output.depth missing target") {
            const auto aspect = row.label.starts_with("output.color") ? "color" : "depth";
            row.code = "render.json_shape_invalid@1";
            row.fields = {{"expected", "a render target name"},
                {"path", std::string{"/rendering/rendering_passes/0/passes/1/output/"} + aspect + "/target"}};
        } else if (row.label == "input element type") {
            row.code = "render.json_shape_invalid@1";
            row.fields = {{"expected", "a string"}, {"path", "/rendering/rendering_passes/0/passes/1/input/0"}};
        } else if (row.label == "empty input") {
            row.code = "render_graph.input_not_produced@1";
            row.fields = {{"graph", "main"}, {"node", "tone"}, {"resource", ""}};
        } else if (row.label == "output.color swapchain subresource") {
            row.provider_revision = true;
        }
    }
    checkPassCompilerRefusals(base, rows);
    SECTION("normalization preserves authored attachment and input aliases") {
        const std::vector<std::pair<std::string, AttachmentLoadOp>> loads{
            {"Clear", AttachmentLoadOp::clear}, {"clear", AttachmentLoadOp::clear},
            {"Load", AttachmentLoadOp::load}, {"load", AttachmentLoadOp::load},
            {"DontCare", AttachmentLoadOp::dont_care}, {"dontCare", AttachmentLoadOp::dont_care}, {"dont_care", AttachmentLoadOp::dont_care}};
        for (const auto &[spelling, expected] : loads) {
            auto authored = base;
            auto &p = authored["rendering_passes"][0]["passes"][1];
            p["input"] = "scene";
            p["output"]["color"] = {{"target", "post"}, {"load_op", spelling}};
            const auto accepted = compilePassFixture(authored);
            const auto definitions = renderingPassDefinitionsOf(*variantOf(accepted, RenderPipelineGraphVariant::flat).render_graph,
                accepted.generation_resources, generationRenderTargetNames(accepted.generation_resources));
            CHECK(definitions.at(0).passes.at(1).colorAttachmentOperations(0).load_op == expected);
            CHECK(definitions.at(0).passes.at(1).input_targets.size() == 1);
        }

        for (const auto &[spelling, expected] : std::vector<std::pair<std::string, AttachmentStoreOp>>{
                {"Store", AttachmentStoreOp::store}, {"store", AttachmentStoreOp::store},
                {"DontCare", AttachmentStoreOp::dont_care}, {"dontCare", AttachmentStoreOp::dont_care}, {"dont_care", AttachmentStoreOp::dont_care}}) {
            auto authored = base;
            auto &p = authored["rendering_passes"][0]["passes"][1];
            p["output"]["color"] = "post";
            p["color_store_op"] = spelling;
            const auto accepted = compilePassFixture(authored);
            const auto definitions = renderingPassDefinitionsOf(*variantOf(accepted, RenderPipelineGraphVariant::flat).render_graph,
                accepted.generation_resources, generationRenderTargetNames(accepted.generation_resources));
            CHECK(definitions.at(0).passes.at(1).colorAttachmentOperations(0).store_op == expected);
        }
    }
}

TEST_CASE("RC-3c2b2 D2 compiler entry retains completed sequence structure and fullscreen state",
          "[rc3c2b2][render-compiler][passes][d2]") {
    const auto base = section(0, false);
    const std::vector<PassCompilerRefusal> rows{
        {"completed graph missing passes", [](Json &r) { r["rendering_passes"][0].erase("passes"); },
            "render_graph.section_invalid@1", {{"graph", "main"}, {"field", "passes"}}, true},
        {"completed graph passes type", [](Json &r) { r["rendering_passes"][0]["passes"] = 1; },
            "render_graph.section_invalid@1", {{"graph", "main"}, {"field", "passes"}}, true},
        {"completed graph duplicate name", [](Json &r) { auto &p = r["rendering_passes"][0]["passes"]; p.push_back(p[1]); },
            "render_graph.duplicate_node@1", {{"graph", "main"}, {"node", "tone"}}, true},
        {"completed pass object", [](Json &r) { r["rendering_passes"][0]["passes"][1] = Json::array(); },
            "render_graph.not_object@1", {{"graph", "main"}, {"field", "passes"}, {"index", 1}}, true},
        {"completed pass missing name", [](Json &r) { r["rendering_passes"][0]["passes"][1].erase("name"); },
            "render_graph.node_name_invalid@1", {{"graph", "main"}, {"field", "passes"}, {"index", 1}}, true},
        {"completed pass name type", [](Json &r) { r["rendering_passes"][0]["passes"][1]["name"] = 1; },
            "render_graph.node_name_invalid@1", {{"graph", "main"}, {"field", "passes"}, {"index", 1}}, true},
        {"completed pass empty name", [](Json &r) { r["rendering_passes"][0]["passes"][1]["name"] = ""; },
            "render_graph.node_name_empty@1", {{"graph", "main"}}, true},
        {"completed pass output missing", [](Json &r) { r["rendering_passes"][0]["passes"][1].erase("output"); },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "tone"}, {"field", "output"}, {"reason", "requires_object"}}, true},
        {"completed fullscreen topology", [](Json &r) { r["rendering_passes"][0]["passes"][1]["raster_state"] = {{"topology", "triangle_strip"}}; },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "tone"}, {"field", "raster_state.topology"}, {"reason", "fullscreen_requires_triangle_list"}}, true},
        {"completed fullscreen state shape", [](Json &r) { r["rendering_passes"][0]["passes"][1]["raster_state"] = {{"typo", true}}; },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "tone"}, {"field", "raster_state"}, {"reason", "invalid"}}, true},
    };
    checkPassCompilerRefusals(base, rows);
    SECTION("shared compute requires an explicit passes array") {
        auto rendering = base;
        rendering["render_targets"][2]["usage"].push_back("STORAGE");
        rendering["compute_tasks"] = Json::array({Json{{"name", "tick"},
            {"shader", "engine://shaders/compute/fixture"}, {"writes", Json::array({"post"})},
            {"dispatch", {{"groups", Json::array({1, 1, 1})}}}}});
        const auto accepted = compilePassFixture(rendering, nullptr,
            [](Json &config) { config["rendering_passes"][0]["passes"] = Json::array(); });
        const auto &graph = variantOf(accepted, RenderPipelineGraphVariant::flat).render_graph->graphs.at(0);
        CHECK(graph.name == "main");
        REQUIRE(graph.nodes.size() == 1);
        CHECK(graph.nodes.at(0).name == "tick");
        const auto definitions = renderingPassDefinitionsOf(*variantOf(accepted, RenderPipelineGraphVariant::flat).render_graph,
            accepted.generation_resources, generationRenderTargetNames(accepted.generation_resources));
        CHECK(definitions.at(0).passes.empty());
        TestSupport::requireNamedRefusal([&] {
            (void)compilePassFixture(rendering, nullptr, [](Json &config) { config["rendering_passes"][0].erase("passes"); });
        }, "render_graph.section_invalid@1", {{"graph", "main"}, {"field", "passes"}});
    }
    SECTION("the completed graph requires its authored name") {
        const auto accepted = compilePassFixture(base);
        CHECK(variantOf(accepted, RenderPipelineGraphVariant::flat).render_graph->graphs.at(0).name == "main");
        TestSupport::requireNamedRefusal([&] {
            (void)compilePassFixture(base, nullptr, [](Json &config) { config["rendering_passes"][0].erase("name"); });
        }, "render_graph.graph_name_invalid@1", {{"index", 0}, {"reason", "missing"}});
    }
}

TEST_CASE("RC-3c2b2 D2 compose leaves fullscreen state to the named IR parser",
          "[rc3c2b2][render-compiler][passes][d2]") {
    const auto base = section(0, false);
    std::vector<PassCompilerRefusal> rows;
    const auto row = [&](const char *name, Json state) {
        rows.push_back({name, [state](Json &r) { r["rendering_passes"][0]["passes"][1]["raster_state"] = state; },
            "render_graph.node_field_invalid@1", {{"graph", "main"}, {"node", "tone"}, {"field", "raster_state"}, {"reason", "invalid"}}, false});
    };
    row("compose state not object", 1);
    row("compose state unknown key", {{"typo", true}});
    row("compose depth ownership", {{"depth_test", true}});
    row("compose state bad blend", {{"blend", "mystery"}});
    row("compose state attachment count", {{"color_attachments", Json::array()}});
    rows.push_back({"compose topology", [](Json &r) { r["rendering_passes"][0]["passes"][1]["raster_state"] = {{"topology", "triangle_strip"}}; },
        "render_graph.node_field_invalid@1", {{"node", "tone"}, {"field", "raster_state.topology"}, {"reason", "fullscreen_requires_triangle_list"}}, false});
    checkPassCompilerRefusals(base, rows);
}

TEST_CASE("RC-3c2b2 D2 routed material state has one parser and completion checks final IR",
          "[rc3c2b2][render-compiler][passes][d2]") {
    auto base = materialCompilerFixture();
    base["render_targets"].erase(base["render_targets"].end()-1);
    auto &passes = base["rendering_passes"][0]["passes"];
    auto &tone = passes[1];
    tone["material_contract"] = "forward_opaque_v1";
    tone["material_outputs"] = {{"schema", "pelican.material_outputs"}, {"version", 1}, {"name", "project.routed"},
        {"outputs", Json::array({Json{{"name", "color"}, {"type", "vec4"}, {"source", "surface.base_color"}}})}};
    tone["material_output_states"] = {{"color", {{"write_mask", "rg"}}}};
    auto deferred = tone;
    deferred["name"] = "deferred";
    deferred["material_contract"] = "deferred_geometry_v1";
    deferred["after"] = Json::array({"draw"});
    tone["after"] = Json::array({"deferred"});
    auto transparent = tone;
    transparent["name"] = "transparent";
    transparent["material_contract"] = "forward_transparent_v1";
    transparent["after"] = Json::array({"tone"});
    passes.insert(passes.begin()+1, deferred);
    passes.insert(passes.end()-1, transparent);
    base["material_routing"] = {{"policy", "hybrid_auto_v1"}, {"routes", {
        {"deferred_geometry", "deferred"}, {"forward_opaque", "tone"}, {"forward_transparent", "transparent"}}}};
    const auto compiled = [&](const Json &rendering, EditingProgram *program = nullptr) {
        return compilePassFixture(rendering, program);
    };
    const auto before = renderGraphParseCount();
    const auto control = compiled(base);
    CHECK(renderGraphParseCount() == before + 1);
    const auto &flat = variantOf(control, RenderPipelineGraphVariant::flat);
    REQUIRE(flat.compiled_pipeline->material_routing);
    const auto &route = flat.compiled_pipeline->material_routing->routes.at(1);
    REQUIRE(route.output_schema);
    CHECK(route.output_states.size() == 1);
    const auto refused = [&](const char *label, std::function<void(Json &)> edit, const char *field) {
        INFO(label);
        auto bad = base;
        edit(bad["rendering_passes"][0]["passes"][2]);
        TestSupport::requireNamedRefusal([&] { (void)compiled(bad); }, "render_graph.node_field_invalid@1",
            {{"graph", "main"}, {"node", "tone"}, {"field", field}, {"reason", "invalid"}});
    };
    refused("compose routing schema type", [](Json &p) { p["material_outputs"] = 1; }, "material_outputs");
    refused("compose routing schema unknown key", [](Json &p) { p["material_outputs"]["typo"] = true; }, "material_outputs");
    auto no_schema = base;
    no_schema["rendering_passes"][0]["passes"][2].erase("material_outputs");
    TestSupport::requireNamedRefusal([&] { (void)compiled(no_schema); }, "render_graph.node_field_invalid@1",
        {{"graph", "main"}, {"node", "tone"}, {"field", "material_output_states"}, {"reason", "requires_material_outputs"}});
    refused("compose routing states type", [](Json &p) { p["material_output_states"] = 1; }, "material_output_states");
    refused("compose routing states unknown output", [](Json &p) { p["material_output_states"]["missing"] = {{"write_mask", "r"}}; }, "material_output_states");
    refused("compose routing states blend", [](Json &p) { p["material_output_states"]["color"]["blend"] = "mystery"; }, "material_output_states");
    const auto route_refusal = [&](Json rendering, const char *graph, const char *node,
                                   const char *field, const char *reason) {
        TestSupport::requireNamedRefusal([&] { (void)compiled(rendering); }, "render_graph.material_route_invalid@1",
            {{"graph", graph}, {"node", node}, {"route", "forward_opaque"}, {"field", field}, {"reason", reason}});
    };
    auto missing = base;
    missing["material_routing"]["routes"]["forward_opaque"] = "missing";
    route_refusal(missing, "main", "missing", "pass", "missing");
    auto non_material = base;
    non_material["material_routing"]["routes"]["forward_opaque"] = "draw";
    route_refusal(non_material, "main", "draw", "pass", "not_material");
    for (const bool omitted : {false, true}) {
        auto contract = base;
        auto &pass = contract["rendering_passes"][0]["passes"][2];
        if (omitted) pass.erase("material_contract");
        else pass["material_contract"] = "forward_transparent_v1";
        route_refusal(contract, "main", "tone", "material_contract", "mismatch");
    }
    for (const auto *field : {"material_outputs", "material_output_states"}) {
        auto different = base;
        auto other = different["rendering_passes"][0];
        other["name"] = "other";
        auto &pass = other["passes"][2];
        if (std::string_view{field} == "material_outputs") pass["material_outputs"]["name"] = "project.other";
        else pass.erase("material_output_states");
        different["rendering_passes"].push_back(other);
        route_refusal(different, "other", "tone", field, "mismatch");
    }
    for (const auto *field : {"material_contract", "material_outputs", "material_output_states"}) {
        INFO(field);
        EditingProgram program;
        program.edit = [field](RenderGraphIR &ir) {
            auto &nodes = ir.graphs.front().nodes;
            auto node = std::find_if(nodes.begin(), nodes.end(), [](const auto &n) { return n.name == "tone"; });
            REQUIRE(node != nodes.end());
            auto &info = std::get<MaterialNodeInfo>(node->info);
            if (std::string_view{field} == "material_contract") info.contract = MaterialPassContract::forward_transparent_v1;
            else if (std::string_view{field} == "material_outputs") info.output_schema->name = "changed";
            else info.output_states.clear();
        };
        TestSupport::requireNamedRefusal([&] { (void)compiled(base, &program); }, "render_graph.material_route_invalid@1",
            {{"graph", "main"}, {"node", "tone"}, {"route", "forward_opaque"}, {"field", field}, {"reason", "mismatch"}});
    }
}

} // namespace Pelican
