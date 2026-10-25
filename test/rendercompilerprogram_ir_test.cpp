// RC-3c1b: the production compiler program (the one RegistrationStages::prepare
// runs, with the variant requests the registration makes: ImGui composed into
// flat, the request-local preview last) reads each completed revision once
// (parseRenderGraph), hands the provider chain's IR on, finalizes the last
// revision once, and refuses by name where an input is first refused.
#include "rendercompilerprogram_ir_test_support.hpp"

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

// RC-3c2b1: a compiler program that returns the engine's output with each
// runtime variant's IR changed by `edit` (what a program of its own may
// return: the common completion makes the compute tasks from it).
struct EditingProgram final : RenderCompilerProgram {
    std::function<void(RenderGraphIR &)> edit;

    RenderCompilerProgramSelection selection(const RenderCompilerBackendContext &backend) const override {
        return defaultVulkanRenderCompilerProgram().selection(backend);
    }
    RenderCompilerProgramOutput compile(const RenderCompilerProgramInput &input) const override {
        auto output = defaultVulkanRenderCompilerProgram().compile(input);
        for (auto &variant : output.variants) {
            if (variant.physical_package == nullptr || variant.render_graph == nullptr) continue;
            auto edited = *variant.render_graph;
            edit(edited);
            variant.render_graph = std::make_shared<const RenderGraphIR>(std::move(edited));
        }
        return output;
    }
};

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
    const auto compiled = [&](std::function<void(RenderGraphIR &)> edit) {
        EditingProgram program;
        program.edit = std::move(edit);
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
        const auto two_graphs = [](RenderGraphIR &ir) {
            auto copy = ir.graphs.front();
            copy.name = "copy";
            sharedNode(copy, "make_args").after.push_back("elsewhere");
            ir.graphs.push_back(std::move(copy));
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

} // namespace Pelican
