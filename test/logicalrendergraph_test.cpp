#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/logicalframegraphadapter.hpp"
#include "../src/project/logicalrendergraph.hpp"
#include "../src/project/logicalrendertype.hpp"
#include "../src/project/materialscreeninput.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

namespace {

void requireThrowsContaining(const std::function<void()> &operation,
                             std::string_view expected) {
    try {
        operation();
        FAIL("operation did not throw");
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string_view{error.what()}.find(expected) !=
                std::string_view::npos);
    }
}

SemanticTypeId maskTypeId() {
    return parseSemanticTypeId("test.render.mask@1");
}

SemanticTypeSchema makeMaskSchema() {
    return SemanticTypeSchema{
        maskTypeId(),
        LogicalTypeConstructor::image,
        {
            TypeParameterSchema{
                "channel", TypeArgumentRole::identity,
                TypeArgumentValueKind::enum_value, EnumValueId{"r"},
                {EnumValueId{"r"}, EnumValueId{"rg"}}},
            TypeParameterSchema{
                "features", TypeArgumentRole::refinement,
                TypeArgumentValueKind::enum_set, EnumValueSet{},
                {EnumValueId{"alpha"}, EnumValueId{"coverage"}}},
            TypeParameterSchema{
                "layers", TypeArgumentRole::identity,
                TypeArgumentValueKind::signed_integer, std::nullopt, {}, 1, 8},
            TypeParameterSchema{
                "scale", TypeArgumentRole::refinement,
                TypeArgumentValueKind::rational, Rational{2, 4}},
            TypeParameterSchema{
                "views", TypeArgumentRole::identity,
                TypeArgumentValueKind::unsigned_integer, std::uint64_t{1}, {},
                std::nullopt, std::nullopt, std::uint64_t{1}, std::uint64_t{4}},
        },
        {"mask_like"},
    };
}

LogicalGraphNode makeReadNode(const LogicalTypeRegistry &registry,
                              const LogicalType &type) {
    LogicalGraphNode node;
    node.name = "sample";
    node.kind = LogicalGraphNodeKind::render;
    node.ports.push_back(LogicalPortContract{
        "source", LogicalPortDirection::input,
        exactLogicalTypePattern(registry, type), {}});
    node.uses.push_back(makeLogicalReadUse(
        "source", LogicalValueId{"scene_color", 0},
        LogicalReadFootprint{LogicalReadFootprintKind::same_pixel, std::nullopt},
        LogicalAccessIntent::sampled));
    return node;
}

LogicalGraphNode makeValueReadNode(const LogicalTypeRegistry &registry,
                                   const LogicalType &type,
                                   std::string node_name,
                                   LogicalValueId value) {
    LogicalGraphNode node;
    node.name = std::move(node_name);
    node.kind = LogicalGraphNodeKind::render;
    node.ports.push_back(LogicalPortContract{
        "source", LogicalPortDirection::input,
        exactLogicalTypePattern(registry, type), {}});
    node.uses.push_back(makeLogicalReadUse(
        "source", std::move(value),
        LogicalReadFootprint{LogicalReadFootprintKind::same_pixel, std::nullopt},
        LogicalAccessIntent::sampled));
    return node;
}

LogicalGraphNode makeValueWriteNode(const LogicalTypeRegistry &registry,
                                    const LogicalType &type,
                                    std::string node_name,
                                    LogicalValueId value) {
    LogicalGraphNode node;
    node.name = std::move(node_name);
    node.kind = LogicalGraphNodeKind::render;
    node.ports.push_back(LogicalPortContract{
        "target", LogicalPortDirection::output,
        exactLogicalTypePattern(registry, type), {}});
    node.uses.push_back(makeLogicalWriteUse(
        "target", std::move(value), LogicalAccessIntent::attachment));
    return node;
}

LogicalConversionImplementation conversionImplementation(
    std::string operation, std::uint64_t provider_identity = 0,
    std::uint32_t provider_generation = 0) {
    return LogicalConversionImplementation{
        std::move(operation), "test.depth_converter@1", provider_identity,
        provider_generation};
}

} // namespace

TEST_CASE("logical type schemas canonicalize defaults and argument order",
          "[logical-render-graph][types]") {
    auto registry = makeBuiltinLogicalTypeRegistry();
    registry.registerSchema(makeMaskSchema());

    const auto first = registry.canonicalize(
        maskTypeId(),
        {{"features", EnumValueSet{{EnumValueId{"coverage"},
                                     EnumValueId{"alpha"},
                                     EnumValueId{"coverage"}}}},
         {"layers", std::int64_t{4}},
         {"scale", Rational{4, 8}}});
    const auto second = registry.canonicalize(
        maskTypeId(),
        {{"scale", Rational{1, 2}},
         {"channel", EnumValueId{"r"}},
         {"layers", std::int64_t{4}},
         {"features", EnumValueSet{{EnumValueId{"alpha"},
                                     EnumValueId{"coverage"}}}}});

    REQUIRE(first == second);
    REQUIRE(canonicalLogicalTypeKey(registry, first) ==
            canonicalLogicalTypeKey(registry, second));
    REQUIRE(canonicalLogicalTypeHash(registry, first) ==
            canonicalLogicalTypeHash(registry, second));
    auto noncanonical = first;
    std::reverse(noncanonical.arguments.begin(), noncanonical.arguments.end());
    requireThrowsContaining(
        [&] { (void)canonicalLogicalTypeKey(registry, noncanonical); },
        "not canonical");
    const Rational half{1, 2};
    const auto scale = std::find_if(first.arguments.begin(), first.arguments.end(),
                                    [](const auto &argument) {
                                        return argument.name == "scale";
                                    });
    REQUIRE(scale != first.arguments.end());
    REQUIRE(std::get<Rational>(scale->value) == half);
    const auto extreme = registry.canonicalize(
        maskTypeId(),
        {{"layers", std::int64_t{1}},
         {"scale", Rational{std::numeric_limits<std::int64_t>::min(),
                            std::uint64_t{1} << 63}}});
    const auto extreme_scale = std::find_if(
        extreme.arguments.begin(), extreme.arguments.end(),
        [](const auto &argument) { return argument.name == "scale"; });
    const Rational negative_one{-1, 1};
    REQUIRE(extreme_scale != extreme.arguments.end());
    REQUIRE(std::get<Rational>(extreme_scale->value) == negative_one);

    requireThrowsContaining(
        [] { (void)parseSemanticTypeId("test.render.mask@01"); },
        "invalid major version");
    requireThrowsContaining(
        [&] { (void)registry.canonicalize(parseSemanticTypeId("test.render.unknown@1")); },
        "unknown logical type schema");
    requireThrowsContaining(
        [&] { (void)registry.canonicalize(maskTypeId()); },
        "missing required logical type argument 'layers'");
    requireThrowsContaining(
        [&] {
            (void)registry.canonicalize(maskTypeId(),
                                        {{"layers", std::int64_t{1}},
                                         {"unknown", std::int64_t{1}}});
        },
        "unknown logical type argument 'unknown'");
    requireThrowsContaining(
        [&] {
            (void)registry.canonicalize(maskTypeId(),
                                        {{"layers", std::int64_t{9}}});
        },
        "above its maximum");
    requireThrowsContaining(
        [&] {
            (void)registry.canonicalize(
                maskTypeId(), {{"layers", std::int64_t{1}},
                               {"channel", EnumValueId{"rgb"}}});
        },
        "unsupported enum value");
}

TEST_CASE("logical type matcher handles exact broad and symbolic constraints",
          "[logical-render-graph][types]") {
    auto registry = makeBuiltinLogicalTypeRegistry();
    registry.registerSchema(makeMaskSchema());
    const auto scene = sceneLinearHdrV1(registry);

    REQUIRE(matchLogicalType(registry, scene,
                             exactLogicalTypePattern(registry, scene)).status ==
            LogicalTypeMatchStatus::exact);
    LogicalTypePattern broad_color;
    broad_color.constructor = LogicalTypeConstructor::image;
    broad_color.required_traits = {"color_like"};
    REQUIRE(matchLogicalType(registry, scene, broad_color).status ==
            LogicalTypeMatchStatus::exact);

    const auto wrong_color = matchLogicalType(
        registry, displayLinearV1(registry),
        exactLogicalTypePattern(registry, scene));
    REQUIRE(wrong_color.status == LogicalTypeMatchStatus::rejected);
    REQUIRE(wrong_color.reason_code == "argument_mismatch");
    const auto semantic_mismatch = matchLogicalType(
        registry,
        registry.canonicalize(maskTypeId(), {{"layers", std::int64_t{1}}}),
        exactLogicalTypePattern(registry, scene));
    REQUIRE(semantic_mismatch.status == LogicalTypeMatchStatus::rejected);
    REQUIRE(semantic_mismatch.reason_code == "semantic_mismatch");

    const auto symbolic = registry.canonicalize(
        parseSemanticTypeId("pelican.render.color_signal@1"),
        {{"reference", SymbolId{"reference_space"}},
         {"transfer", EnumValueId{"linear"}},
         {"range", EnumValueId{"extended"}}});
    const auto deferred =
        matchLogicalType(registry, symbolic,
                         exactLogicalTypePattern(registry, scene));
    REQUIRE(deferred.status == LogicalTypeMatchStatus::deferred);
    REQUIRE(deferred.bindings.size() == 1);
    REQUIRE(deferred.bindings.front().symbol.value == "reference_space");

    const auto mask = registry.canonicalize(
        maskTypeId(),
        {{"layers", std::int64_t{4}},
         {"features", EnumValueSet{{EnumValueId{"alpha"},
                                     EnumValueId{"coverage"}}}}});
    LogicalTypePattern mask_pattern;
    mask_pattern.constructor = LogicalTypeConstructor::image;
    mask_pattern.semantic = maskTypeId();
    mask_pattern.required_traits = {"mask_like"};
    mask_pattern.predicates = {
        TypeArgumentOneOf{"channel",
                          {EnumValueId{"r"}, EnumValueId{"rg"}}},
        TypeArgumentSignedRange{"layers", 2, 6},
        TypeArgumentUnsignedRange{"views", 1, 2},
        TypeArgumentRationalRange{"scale", Rational{1, 4}, Rational{3, 4}},
        TypeArgumentSetContains{"features",
                                EnumValueSet{{EnumValueId{"coverage"}}}},
    };
    REQUIRE(matchLogicalType(registry, mask, mask_pattern).status ==
            LogicalTypeMatchStatus::exact);

    LogicalTypePattern exact_rational_boundary;
    exact_rational_boundary.semantic = maskTypeId();
    exact_rational_boundary.predicates = {TypeArgumentRationalRange{
        "scale",
        Rational{std::numeric_limits<std::int64_t>::min(),
                 std::uint64_t{1} << 63},
        Rational{std::numeric_limits<std::int64_t>::max(),
                 std::numeric_limits<std::uint64_t>::max()}}};
    const auto outside_exact_boundary =
        matchLogicalType(registry, mask, exact_rational_boundary);
    REQUIRE(outside_exact_boundary.status == LogicalTypeMatchStatus::rejected);
    REQUIRE(outside_exact_boundary.reason_code == "argument_out_of_range");
}

TEST_CASE("logical conversions are finite deterministic and reject ambiguity",
          "[logical-render-graph][types]") {
    auto registry = makeBuiltinLogicalTypeRegistry();
    const auto device = deviceDepthV1(registry);
    const auto linear = linearViewDepthV1(registry);
    const auto depth_id = parseSemanticTypeId("pelican.render.depth@1");
    const auto intermediate = registry.canonicalize(
        depth_id,
        {{"representation", EnumValueId{"linear_distance"}},
         {"space", EnumValueId{"projection"}}});

    LogicalTypeConversionRegistry conversions;
    conversions.registerConversion(
        registry, {"test.depth.decode@1", device, intermediate,
                   LogicalConversionMode::automatic_safe, 1,
                   conversionImplementation("test.depth.decode@1")});
    conversions.registerConversion(
        registry, {"test.depth.to_view@1", intermediate, linear,
                   LogicalConversionMode::automatic_safe, 1,
                   conversionImplementation("test.depth.to_view@1")});
    const auto converted =
        conversions.match(registry, device,
                          exactLogicalTypePattern(registry, linear));
    REQUIRE(converted.status == LogicalTypeMatchStatus::convertible);
    const std::vector<std::string> expected_path{"test.depth.decode@1",
                                                 "test.depth.to_view@1"};
    REQUIRE(converted.conversion_path == expected_path);
    REQUIRE(conversions.conversion("test.depth.decode@1").implementation ==
            conversionImplementation("test.depth.decode@1"));

    conversions.registerConversion(
        registry, {"test.depth.explicit_linearize@1", device, linear,
                   LogicalConversionMode::explicit_only, 1,
                   conversionImplementation(
                       "test.depth.explicit_linearize@1")});
    REQUIRE(conversions.match(registry, device,
                              exactLogicalTypePattern(registry, linear))
                .conversion_path == expected_path);
    const auto explicit_match = conversions.match(
        registry, device, exactLogicalTypePattern(registry, linear), true);
    REQUIRE(explicit_match.status == LogicalTypeMatchStatus::convertible);
    const std::vector<std::string> explicit_path{
        "test.depth.explicit_linearize@1"};
    REQUIRE(explicit_match.conversion_path == explicit_path);

    LogicalTypeConversionRegistry explicit_only;
    explicit_only.registerConversion(
        registry, {"test.depth.explicit_only@1", device, linear,
                   LogicalConversionMode::explicit_only, 1,
                   conversionImplementation("test.depth.explicit_only@1")});
    const auto explicit_rejected = explicit_only.match(
        registry, device, exactLogicalTypePattern(registry, linear));
    REQUIRE(explicit_rejected.status == LogicalTypeMatchStatus::rejected);
    REQUIRE(explicit_rejected.reason_code == "no_conversion");
    REQUIRE(explicit_only
                .match(registry, device,
                       exactLogicalTypePattern(registry, linear), true)
                .status == LogicalTypeMatchStatus::convertible);

    LogicalTypeConversionRegistry ambiguous;
    ambiguous.registerConversion(
        registry, {"test.depth.linearize_a@1", device, linear,
                   LogicalConversionMode::automatic_safe, 1,
                   conversionImplementation("test.depth.linearize_a@1")});
    ambiguous.registerConversion(
        registry, {"test.depth.linearize_b@1", device, linear,
                   LogicalConversionMode::automatic_safe, 1,
                   conversionImplementation("test.depth.linearize_b@1")});
    const auto ambiguous_match =
        ambiguous.match(registry, device,
                        exactLogicalTypePattern(registry, linear));
    REQUIRE(ambiguous_match.status == LogicalTypeMatchStatus::rejected);
    REQUIRE(ambiguous_match.reason_code == "ambiguous_conversion");
    REQUIRE(ambiguous_match.detail.find("test.depth.linearize_a@1") !=
            std::string::npos);
    REQUIRE(ambiguous_match.detail.find("test.depth.linearize_b@1") !=
            std::string::npos);
    requireThrowsContaining(
        [&] {
            LogicalTypeConversionRegistry invalid;
            invalid.registerConversion(
                registry, {"test.depth.unversioned", device, linear,
                           LogicalConversionMode::automatic_safe, 1,
                           conversionImplementation(
                               "test.depth.invalid_operation@1")});
        },
        "namespace.name@major");
    requireThrowsContaining(
        [&] {
            LogicalTypeConversionRegistry invalid;
            invalid.registerConversion(
                registry, {"test.depth.missing_implementation@1", device,
                           linear, LogicalConversionMode::automatic_safe, 1,
                           {}});
        },
        "logical conversion operation id must not be empty");
    requireThrowsContaining(
        [&] {
            LogicalTypeConversionRegistry invalid;
            invalid.registerConversion(
                registry, {"test.depth.partial_provider_identity@1", device,
                           linear, LogicalConversionMode::automatic_safe, 1,
                           conversionImplementation(
                               "test.depth.partial_provider_identity@1", 7,
                               0)});
        },
        "identity and generation must both be zero");
    requireThrowsContaining(
        [&] { (void)conversions.conversion("test.depth.missing@1"); },
        "is not registered");
}

TEST_CASE("builtin hybrid screen inputs carry typed footprints and explicit display conversions",
          "[logical-render-graph][types][screen-input][rpe6b1]") {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto conversions = makeBuiltinLogicalTypeConversionRegistry(types);

    const auto refraction =
        makeBuiltinMaterialScreenInputContract(types, "opaque_color");
    REQUIRE(refraction.source_type == sceneLinearHdrV1(types));
    REQUIRE(refraction.sampled_type == sceneLinearHdrV1(types));
    REQUIRE(refraction.footprint.kind ==
            LogicalReadFootprintKind::neighborhood);
    REQUIRE_FALSE(refraction.conversion.has_value());

    auto depth_fade =
        makeBuiltinMaterialScreenInputContract(types, "linear_view_depth");
    REQUIRE(depth_fade.source_type == deviceDepthV1(types));
    REQUIRE(depth_fade.sampled_type == linearViewDepthV1(types));
    REQUIRE(depth_fade.footprint.kind ==
            LogicalReadFootprintKind::same_pixel);
    const auto resolved_depth = resolveMaterialScreenInputContract(
        types, conversions, depth_fade, deviceDepthV1(types));
    REQUIRE(resolved_depth.conversion_path ==
            std::vector<std::string>{"pelican.render.depth_linearize@1"});

    const auto tone_pattern =
        exactLogicalTypePattern(types, displayLinearV1(types));
    const auto implicit_tone =
        conversions.match(types, sceneLinearHdrV1(types), tone_pattern);
    REQUIRE(implicit_tone.status == LogicalTypeMatchStatus::rejected);
    const auto explicit_tone = conversions.match(
        types, sceneLinearHdrV1(types), tone_pattern, true);
    REQUIRE(explicit_tone.status == LogicalTypeMatchStatus::convertible);
    REQUIRE(explicit_tone.conversion_path ==
            std::vector<std::string>{"pelican.render.tone_map@1"});

    REQUIRE_THROWS_WITH(
        resolveMaterialScreenInputContract(
            types, conversions, std::move(depth_fade),
            sceneLinearHdrV1(types)),
        Catch::Matchers::ContainsSubstring("linear_view_depth") &&
            Catch::Matchers::ContainsSubstring("source type"));
    REQUIRE_THROWS_WITH(
        makeBuiltinMaterialScreenInputContract(types, "unknown_history"),
        Catch::Matchers::ContainsSubstring("unknown_history"));
}

TEST_CASE("logical graph validates port types footprints and relations",
          "[logical-render-graph][validation]") {
    const auto registry = makeBuiltinLogicalTypeRegistry();
    const auto scene = sceneLinearHdrV1(registry);
    CompiledLogicalRenderGraph graph{
        "typed_graph",
        {LogicalResourceDesc{"scene_color", scene,
                             LogicalMaterializationRequirement::preferred}},
        {LogicalValueImport{LogicalValueId{"scene_color", 0},
                            LogicalValueImportKind::graph_input}},
        {makeReadNode(registry, scene)},
        {},
    };
    validateCompiledLogicalRenderGraph(registry, graph);
    const auto dump = compiledLogicalRenderGraphToJson(graph);
    REQUIRE(dump.at("schema") == "pelican.logical_render_graph");
    REQUIRE(dump.at("version") == 2);

    auto type_mismatch = graph;
    type_mismatch.nodes.front().ports.front().accepted_type =
        exactLogicalTypePattern(registry, displayLinearV1(registry));
    requireThrowsContaining(
        [&] { validateCompiledLogicalRenderGraph(registry, type_mismatch); },
        "argument_mismatch");

    auto missing_footprint = graph;
    missing_footprint.nodes.front().uses.front().footprint = {};
    requireThrowsContaining(
        [&] { validateCompiledLogicalRenderGraph(registry, missing_footprint); },
        "requires a footprint");

    auto unknown_resource = graph;
    unknown_resource.nodes.front().uses.front().input_value->resource = "missing";
    requireThrowsContaining(
        [&] { validateCompiledLogicalRenderGraph(registry, unknown_resource); },
        "unknown resource 'missing'");

    auto unknown_relation = graph;
    unknown_relation.nodes.front().ports.front().relations.push_back(
        LogicalPortRelation{LogicalPortRelationKind::same_extent, "missing"});
    requireThrowsContaining(
        [&] { validateCompiledLogicalRenderGraph(registry, unknown_relation); },
        "unknown port 'missing'");
}

TEST_CASE("logical value versions derive producer edges without declaration-order semantics",
          "[logical-render-graph][values]") {
    const auto registry = makeBuiltinLogicalTypeRegistry();
    const auto scene = sceneLinearHdrV1(registry);
    const auto value = LogicalValueId{"scene_color", 1};

    auto consumer = makeValueReadNode(registry, scene, "consumer", value);
    consumer.declaration_index = 1;
    auto producer = makeValueWriteNode(registry, scene, "producer", value);
    producer.declaration_index = 0;
    CompiledLogicalRenderGraph graph{
        "versioned",
        {LogicalResourceDesc{"scene_color", scene,
                             LogicalMaterializationRequirement::virtual_resource}},
        {},
        {consumer, producer},
        {},
    };

    validateCompiledLogicalRenderGraph(registry, graph);
    const auto edges = deriveLogicalDataEdges(graph);
    REQUIRE(edges == std::vector{LogicalDataEdge{
                         value, "producer", "target", "consumer", "source"}});
    const auto dump = compiledLogicalRenderGraphToJson(graph);
    REQUIRE(dump.at("data_edges").size() == 1);
    REQUIRE(dump.at("data_edges").at(0).at("producer").at("node") ==
            "producer");
    REQUIRE(dump.at("data_edges").at(0).at("consumer").at("node") ==
            "consumer");

    auto duplicate = graph;
    duplicate.nodes.push_back(
        makeValueWriteNode(registry, scene, "other_producer", value));
    requireThrowsContaining(
        [&] { validateCompiledLogicalRenderGraph(registry, duplicate); },
        "multiple producers");

    auto missing = graph;
    missing.nodes.erase(missing.nodes.begin() + 1);
    requireThrowsContaining(
        [&] { validateCompiledLogicalRenderGraph(registry, missing); },
        "no producer or import");

    auto cyclic = graph;
    cyclic.nodes.at(1).after = {"consumer"};
    requireThrowsContaining(
        [&] { validateCompiledLogicalRenderGraph(registry, cyclic); },
        "dependency cycle");
}

TEST_CASE("logical graph keeps nominal connections and fast-default independence explicit",
          "[logical-render-graph][values][policy]") {
    const auto registry = makeBuiltinLogicalTypeRegistry();
    const auto scene = sceneLinearHdrV1(registry);
    CompiledLogicalRenderGraph independent{
        "independent",
        {LogicalResourceDesc{"left", scene,
                             LogicalMaterializationRequirement::virtual_resource},
         LogicalResourceDesc{"right", scene,
                             LogicalMaterializationRequirement::virtual_resource}},
        {},
        {makeValueWriteNode(registry, scene, "left_writer", {"left", 1}),
         makeValueWriteNode(registry, scene, "right_writer", {"right", 1})},
        {},
    };
    validateCompiledLogicalRenderGraph(registry, independent);
    REQUIRE(deriveLogicalDataEdges(independent).empty());

    CompiledLogicalRenderGraph independently_versioned{
        "independently_versioned",
        {LogicalResourceDesc{
            "scene_color", scene,
            LogicalMaterializationRequirement::virtual_resource}},
        {},
        {makeValueWriteNode(registry, scene, "version_one_writer",
                            {"scene_color", 1}),
         makeValueWriteNode(registry, scene, "version_two_writer",
                            {"scene_color", 2})},
        {},
    };
    validateCompiledLogicalRenderGraph(registry, independently_versioned);
    REQUIRE(deriveLogicalDataEdges(independently_versioned).empty());

    auto trait_only = independent;
    trait_only.nodes.front().ports.front().accepted_type = LogicalTypePattern{
        LogicalTypeConstructor::image, std::nullopt, {"color_like"}, {}};
    requireThrowsContaining(
        [&] { validateCompiledLogicalRenderGraph(registry, trait_only); },
        "requires a nominal semantic type");

    auto invalid_intent = independent;
    invalid_intent.nodes.front().uses.front().intent =
        LogicalAccessIntent::sampled;
    requireThrowsContaining(
        [&] { validateCompiledLogicalRenderGraph(registry, invalid_intent); },
        "intent sampled rejects access write");
}

TEST_CASE("logical shadow adapter preserves declared read footprints",
          "[logical-render-graph][shadow][footprint]") {
    const auto registry =
        makeBuiltinLogicalTypeRegistry();
    FrameGraphDefinition source{
        .name = "typed_shadow",
        .declared_resources = {"gbuffer"},
        .nodes = {
            FrameGraphNodeDefinition{
                .name = "geometry",
                .kind = FramePlanNodeKind::render,
                .declaration_index = 0,
                .writes = {"gbuffer"},
            },
            FrameGraphNodeDefinition{
                .name = "lighting",
                .kind = FramePlanNodeKind::render,
                .declaration_index = 1,
                .reads = {"gbuffer"},
                .read_footprints = {
                    {"gbuffer",
                     {LogicalReadFootprintKind::same_pixel,
                      std::nullopt}},
                },
                .writes = {"swapchain"},
            },
        },
    };

    const auto graph =
        compileLogicalFrameGraphShadow(
            source, registry);
    const auto &lighting = graph.nodes.at(1);
    const auto read = std::find_if(
        lighting.uses.begin(), lighting.uses.end(),
        [](const auto &use) {
            return use.input_value &&
                   use.input_value->resource ==
                       "gbuffer";
        });
    REQUIRE(read != lighting.uses.end());
    REQUIRE(
        read->footprint ==
        LogicalReadFootprint{
            LogicalReadFootprintKind::same_pixel,
            std::nullopt});
    REQUIRE(std::none_of(
        graph.decisions.begin(),
        graph.decisions.end(),
        [](const auto &decision) {
            return decision.code ==
                   "legacy_read_footprint_conservative";
        }));

    source.nodes.at(1).read_footprints.push_back(
        {"missing",
         {LogicalReadFootprintKind::same_pixel,
          std::nullopt}});
    REQUIRE_THROWS_AS(
        compileLogicalFrameGraphShadow(
            source, registry),
        std::runtime_error);
}

TEST_CASE("legacy frame graph compiles to a diagnostic-only logical shadow",
          "[logical-render-graph][shadow]") {
    const auto registry = makeBuiltinLogicalTypeRegistry();
    FrameGraphDefinition source;
    source.name = "legacy";
    source.declared_resources = {"scene_color", "main_depth", "history_color"};
    source.history_resources = {"history_color"};
    source.nodes = {
        FrameGraphNodeDefinition{
            .name = "opaque",
            .kind = FramePlanNodeKind::render,
            .declaration_index = 0,
            .writes = {"scene_color", "main_depth"},
        },
        FrameGraphNodeDefinition{
            .name = "composite",
            .kind = FramePlanNodeKind::render,
            .declaration_index = 1,
            .reads = {"scene_color", "main_depth"},
            .reads_history = {"history_color"},
            .writes = {"scene_color"},
            .after = {"opaque"},
        },
        FrameGraphNodeDefinition{
            .name = "present",
            .kind = FramePlanNodeKind::render,
            .declaration_index = 2,
            .reads = {"scene_color"},
            .writes = {"swapchain"},
            .after = {"composite"},
        },
    };

    const auto before = framePlanToJson(planFrameGraph(source));
    const LogicalFrameGraphShadowOptions options{
        {
            LogicalShadowResourceType{"scene_color", sceneLinearHdrV1(registry),
                                      LogicalMaterializationRequirement::preferred},
            LogicalShadowResourceType{"main_depth", deviceDepthV1(registry),
                                      std::nullopt},
        },
        true,
    };
    const auto first = compileLogicalFrameGraphShadow(source, registry, options);
    const auto second = compileLogicalFrameGraphShadow(source, registry, options);
    const auto after = framePlanToJson(planFrameGraph(source));

    REQUIRE(before == after);
    REQUIRE(compiledLogicalRenderGraphToJson(first) ==
            compiledLogicalRenderGraphToJson(second));
    REQUIRE(first.resources.at(2).materialization ==
            LogicalMaterializationRequirement::required);
    REQUIRE(first.resources.at(2).type == legacyOpaqueResourceV1(registry));
    REQUIRE(first.resources.at(3).name == "swapchain");
    REQUIRE(first.resources.at(3).materialization ==
            LogicalMaterializationRequirement::external);
    const auto &composite = first.nodes.at(1);
    const auto regular_read = std::find_if(
        composite.uses.begin(), composite.uses.end(), [](const auto &use) {
            return use.input_value &&
                   use.input_value->resource == "scene_color" &&
                   use.access == LogicalAccessMode::read;
        });
    const auto history_read = std::find_if(
        composite.uses.begin(), composite.uses.end(), [](const auto &use) {
            return use.input_value &&
                   use.input_value->resource == "history_color";
        });
    REQUIRE(regular_read != composite.uses.end());
    REQUIRE(regular_read->input_value->version == 1);
    REQUIRE(regular_read->footprint.kind == LogicalReadFootprintKind::arbitrary);
    REQUIRE(history_read != composite.uses.end());
    REQUIRE(history_read->input_value->version == 0);
    REQUIRE(history_read->footprint.kind == LogicalReadFootprintKind::temporal);
    const auto composite_write = std::find_if(
        composite.uses.begin(), composite.uses.end(), [](const auto &use) {
            return use.output_value &&
                   use.output_value->resource == "scene_color";
        });
    REQUIRE(composite_write != composite.uses.end());
    REQUIRE(composite_write->output_value->version == 2);
    const auto present_read = std::find_if(
        first.nodes.at(2).uses.begin(), first.nodes.at(2).uses.end(),
        [](const auto &use) {
            return use.input_value &&
                   use.input_value->resource == "scene_color";
        });
    REQUIRE(present_read != first.nodes.at(2).uses.end());
    REQUIRE(present_read->input_value->version == 2);
    const auto data_edges = deriveLogicalDataEdges(first);
    REQUIRE(std::any_of(data_edges.begin(), data_edges.end(), [](const auto &edge) {
        return edge.value == LogicalValueId{"scene_color", 1} &&
               edge.producer_node == "opaque" &&
               edge.consumer_node == "composite";
    }));
    REQUIRE(std::any_of(data_edges.begin(), data_edges.end(), [](const auto &edge) {
        return edge.value == LogicalValueId{"scene_color", 2} &&
               edge.producer_node == "composite" &&
               edge.consumer_node == "present";
    }));
    REQUIRE(std::any_of(first.imports.begin(), first.imports.end(),
                        [](const auto &imported) {
                            return imported.value ==
                                       LogicalValueId{"history_color", 0} &&
                                   imported.kind ==
                                       LogicalValueImportKind::previous_epoch;
                        }));
    REQUIRE(std::any_of(first.imports.begin(), first.imports.end(),
                        [](const auto &imported) {
                            return imported.value ==
                                       LogicalValueId{"scene_color", 0} &&
                                   imported.kind ==
                                       LogicalValueImportKind::legacy_implicit;
                        }));
    REQUIRE(std::any_of(first.decisions.begin(), first.decisions.end(),
                        [](const auto &decision) {
                            return decision.code == "shadow_graph_only";
                        }));
    REQUIRE(std::any_of(first.decisions.begin(), first.decisions.end(),
                        [](const auto &decision) {
                            return decision.code == "legacy_type_fallback" &&
                                   decision.subject == "history_color";
                        }));
    REQUIRE(std::any_of(first.decisions.begin(), first.decisions.end(),
                        [](const auto &decision) {
                            return decision.code == "legacy_external_resource" &&
                                   decision.subject == "swapchain";
                        }));

    auto strict = options;
    strict.allow_legacy_type_fallback = false;
    requireThrowsContaining(
        [&] { (void)compileLogicalFrameGraphShadow(source, registry, strict); },
        "lacks an explicit type: history_color");

    FrameGraphDefinition snapshot;
    snapshot.name = "snapshot";
    snapshot.declared_resources = {"scene_color"};
    snapshot.nodes = {FrameGraphNodeDefinition{
        .name = "copy",
        .kind = FramePlanNodeKind::snapshot_copy,
        .declaration_index = 0,
        .reads = {"scene_color"},
        .writes = {"captured_color"},
        .snapshot_after = "opaque",
        .byte_size = 4096,
    }};
    const auto snapshot_shadow =
        compileLogicalFrameGraphShadow(snapshot, registry);
    REQUIRE(std::any_of(snapshot_shadow.decisions.begin(),
                        snapshot_shadow.decisions.end(), [](const auto &decision) {
                            return decision.code == "legacy_snapshot_point" &&
                                   decision.subject == "copy" &&
                                   decision.detail == "opaque";
                        }));
    REQUIRE(std::any_of(snapshot_shadow.decisions.begin(),
                        snapshot_shadow.decisions.end(), [](const auto &decision) {
                            return decision.code ==
                                       "legacy_physical_byte_size_omitted" &&
                                   decision.subject == "copy" &&
                                   decision.detail.find("4096") !=
                                       std::string::npos;
                        }));
}

} // namespace Pelican
