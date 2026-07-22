#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/logicalframegraphadapter.hpp"
#include "../src/project/logicalrendergraph.hpp"
#include "../src/project/logicalrendertype.hpp"

#include <catch2/catch_test_macros.hpp>

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
    node.uses.push_back(LogicalResourceUse{
        "source", "scene_color", LogicalAccessMode::read,
        LogicalReadFootprint{LogicalReadFootprintKind::same_pixel, std::nullopt}});
    return node;
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
                   LogicalConversionMode::automatic_safe, 1});
    conversions.registerConversion(
        registry, {"test.depth.to_view@1", intermediate, linear,
                   LogicalConversionMode::automatic_safe, 1});
    const auto converted =
        conversions.match(registry, device,
                          exactLogicalTypePattern(registry, linear));
    REQUIRE(converted.status == LogicalTypeMatchStatus::convertible);
    const std::vector<std::string> expected_path{"test.depth.decode@1",
                                                 "test.depth.to_view@1"};
    REQUIRE(converted.conversion_path == expected_path);

    conversions.registerConversion(
        registry, {"test.depth.explicit_linearize@1", device, linear,
                   LogicalConversionMode::explicit_only, 1});
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
                   LogicalConversionMode::explicit_only, 1});
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
                   LogicalConversionMode::automatic_safe, 1});
    ambiguous.registerConversion(
        registry, {"test.depth.linearize_b@1", device, linear,
                   LogicalConversionMode::automatic_safe, 1});
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
                           LogicalConversionMode::automatic_safe, 1});
        },
        "namespace.name@major");
}

TEST_CASE("logical graph validates port types footprints and relations",
          "[logical-render-graph][validation]") {
    const auto registry = makeBuiltinLogicalTypeRegistry();
    const auto scene = sceneLinearHdrV1(registry);
    CompiledLogicalRenderGraph graph{
        "typed_graph",
        {LogicalResourceDesc{"scene_color", scene,
                             LogicalMaterializationRequirement::preferred}},
        {makeReadNode(registry, scene)},
        {},
    };
    validateCompiledLogicalRenderGraph(registry, graph);
    const auto dump = compiledLogicalRenderGraphToJson(graph);
    REQUIRE(dump.at("schema") == "pelican.logical_render_graph");
    REQUIRE(dump.at("version") == 1);

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
    unknown_resource.nodes.front().uses.front().resource = "missing";
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

TEST_CASE("legacy frame graph compiles to a diagnostic-only logical shadow",
          "[logical-render-graph][shadow]") {
    const auto registry = makeBuiltinLogicalTypeRegistry();
    FrameGraphDefinition source;
    source.name = "legacy";
    source.declared_resources = {"scene_color", "main_depth", "history_color"};
    source.history_resources = {"history_color"};
    source.nodes = {
        FrameGraphNodeDefinition{"opaque", FramePlanNodeKind::render, 0, {}, {},
                                 {"scene_color", "main_depth"}, {}, {}, {}, 0},
        FrameGraphNodeDefinition{"composite", FramePlanNodeKind::render, 1,
                                 {"scene_color", "main_depth"}, {"history_color"},
                                 {"scene_color"}, {"opaque"}, {}, {}, 0},
        FrameGraphNodeDefinition{"present", FramePlanNodeKind::render, 2,
                                 {"scene_color"}, {}, {"swapchain"},
                                 {"composite"}, {}, {}, 0},
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
            return use.resource == "scene_color" &&
                   use.access == LogicalAccessMode::read;
        });
    const auto history_read = std::find_if(
        composite.uses.begin(), composite.uses.end(), [](const auto &use) {
            return use.resource == "history_color";
        });
    REQUIRE(regular_read != composite.uses.end());
    REQUIRE(regular_read->footprint.kind == LogicalReadFootprintKind::arbitrary);
    REQUIRE(history_read != composite.uses.end());
    REQUIRE(history_read->footprint.kind == LogicalReadFootprintKind::temporal);
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
        "copy", FramePlanNodeKind::snapshot_copy, 0, {"scene_color"}, {},
        {"captured_color"}, {}, {}, "opaque", 4096}};
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
