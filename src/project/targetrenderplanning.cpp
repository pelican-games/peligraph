#include "targetrenderplanning.hpp"
#include "imagesubresourcejson.hpp"
#include "stablefingerprint.hpp"
#include "vulkancompletephysicalplan.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

constexpr std::string_view kGraphicsCapability =
    "pelican.vulkan.graphics@1";
constexpr std::string_view kSampledImageCapability =
    "pelican.vulkan.sampled_image@1";
constexpr std::string_view kStorageBufferCapability =
    "pelican.vulkan.storage_buffer@1";
constexpr std::string_view kTransferCopyCapability =
    "pelican.vulkan.transfer_copy@1";
constexpr std::string_view kTileBasedCapability =
    "pelican.vulkan.tile_based@1";
constexpr std::string_view kLocalReadCapability =
    "pelican.vulkan.dynamic_rendering_local_read@1";
constexpr std::string_view kTransientAttachmentCapability =
    "pelican.vulkan.transient_attachment@1";
constexpr std::string_view kColorAttachmentBudgetFact =
    "pelican.vulkan.max_color_attachments@1";
constexpr std::string_view kMaterializedCandidate =
    "pelican.vulkan.materialized_plan@1";
constexpr std::string_view kTransientCandidate =
    "pelican.vulkan.transient_plan@1";
constexpr std::string_view kTileLocalCandidate =
    "pelican.vulkan.tile_local_plan@1";
constexpr std::string_view kPinPackageSchema =
    "pelican.vulkan_target_plan_pins";
constexpr std::uint32_t kPinPackageVersion = 1;

void appendFingerprintBoolean(
    StableFingerprint64 &fingerprint, bool value) {
    fingerprint.appendUnsigned(value ? 1u : 0u);
}

void appendFingerprintSigned(
    StableFingerprint64 &fingerprint, std::int64_t value) {
    fingerprint.appendUnsigned(
        static_cast<std::uint64_t>(value));
}

template <typename Range, typename Append>
void appendFingerprintRange(
    StableFingerprint64 &fingerprint,
    const Range &values, Append append) {
    fingerprint.appendUnsigned(values.size());
    for (const auto &value : values) {
        append(fingerprint, value);
    }
}

template <typename Range>
void appendFingerprintStrings(
    StableFingerprint64 &fingerprint,
    const Range &values) {
    appendFingerprintRange(
        fingerprint, values,
        [](StableFingerprint64 &target,
           const auto &value) {
            target.appendString(value);
        });
}

template <typename Value, typename Append>
void appendFingerprintOptional(
    StableFingerprint64 &fingerprint,
    const std::optional<Value> &value,
    Append append) {
    appendFingerprintBoolean(
        fingerprint, value.has_value());
    if (value) append(fingerprint, *value);
}

void appendSemanticTypeIdFingerprint(
    StableFingerprint64 &fingerprint,
    const SemanticTypeId &id) {
    fingerprint.appendString(id.name_space);
    fingerprint.appendString(id.name);
    fingerprint.appendUnsigned(id.major_version);
}

void appendRationalFingerprint(
    StableFingerprint64 &fingerprint,
    const Rational &value) {
    appendFingerprintSigned(
        fingerprint, value.numerator);
    fingerprint.appendUnsigned(value.denominator);
}

template <typename>
inline constexpr bool unsupportedFingerprintValue = false;

void appendTypeArgumentValueFingerprint(
    StableFingerprint64 &fingerprint,
    const TypeArgumentValue &value) {
    fingerprint.appendString(
        typeArgumentValueKindName(
            typeArgumentValueKind(value)));
    std::visit(
        [&](const auto &entry) {
            using Entry =
                std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<Entry, bool>) {
                appendFingerprintBoolean(
                    fingerprint, entry);
            } else if constexpr (
                std::is_same_v<Entry, std::int64_t>) {
                appendFingerprintSigned(
                    fingerprint, entry);
            } else if constexpr (
                std::is_same_v<Entry, std::uint64_t>) {
                fingerprint.appendUnsigned(entry);
            } else if constexpr (
                std::is_same_v<Entry, Rational>) {
                appendRationalFingerprint(
                    fingerprint, entry);
            } else if constexpr (
                std::is_same_v<Entry, EnumValueId> ||
                std::is_same_v<Entry, SymbolId>) {
                fingerprint.appendString(entry.value);
            } else if constexpr (
                std::is_same_v<Entry, SemanticTypeId>) {
                appendSemanticTypeIdFingerprint(
                    fingerprint, entry);
            } else if constexpr (
                std::is_same_v<Entry, IntegerInterval>) {
                appendFingerprintSigned(
                    fingerprint, entry.minimum);
                appendFingerprintSigned(
                    fingerprint, entry.maximum);
            } else if constexpr (
                std::is_same_v<Entry, EnumValueSet>) {
                appendFingerprintRange(
                    fingerprint, entry.values,
                    [](StableFingerprint64 &target,
                       const EnumValueId &item) {
                        target.appendString(item.value);
                    });
            } else {
                static_assert(
                    unsupportedFingerprintValue<Entry>);
            }
        },
        value);
}

void appendLogicalTypeFingerprint(
    StableFingerprint64 &fingerprint,
    const LogicalType &type) {
    fingerprint.appendString(
        logicalTypeConstructorName(type.constructor));
    appendSemanticTypeIdFingerprint(
        fingerprint, type.semantic);
    appendFingerprintRange(
        fingerprint, type.arguments,
        [](StableFingerprint64 &target,
           const LogicalTypeArgument &argument) {
            target.appendString(argument.name);
            target.appendString(
                typeArgumentRoleName(argument.role));
            appendTypeArgumentValueFingerprint(
                target, argument.value);
        });
}

void appendLogicalTypePatternFingerprint(
    StableFingerprint64 &fingerprint,
    const LogicalTypePattern &pattern) {
    appendFingerprintOptional(
        fingerprint, pattern.constructor,
        [](StableFingerprint64 &target,
           LogicalTypeConstructor constructor) {
            target.appendString(
                logicalTypeConstructorName(constructor));
        });
    appendFingerprintOptional(
        fingerprint, pattern.semantic,
        appendSemanticTypeIdFingerprint);
    appendFingerprintStrings(
        fingerprint, pattern.required_traits);
    appendFingerprintRange(
        fingerprint, pattern.predicates,
        [](StableFingerprint64 &target,
           const TypeArgumentPredicate &predicate) {
            std::visit(
                [&](const auto &entry) {
                    using Entry = std::decay_t<
                        decltype(entry)>;
                    target.appendString(entry.name);
                    if constexpr (std::is_same_v<
                                      Entry,
                                      TypeArgumentEquals>) {
                        target.appendString("equals");
                        appendTypeArgumentValueFingerprint(
                            target, entry.expected);
                    } else if constexpr (std::is_same_v<
                                             Entry,
                                             TypeArgumentOneOf>) {
                        target.appendString("one_of");
                        appendFingerprintRange(
                            target, entry.allowed,
                            appendTypeArgumentValueFingerprint);
                    } else if constexpr (std::is_same_v<
                                             Entry,
                                             TypeArgumentSignedRange>) {
                        target.appendString("signed_range");
                        appendFingerprintSigned(
                            target, entry.minimum);
                        appendFingerprintSigned(
                            target, entry.maximum);
                    } else if constexpr (std::is_same_v<
                                             Entry,
                                             TypeArgumentUnsignedRange>) {
                        target.appendString("unsigned_range");
                        target.appendUnsigned(entry.minimum);
                        target.appendUnsigned(entry.maximum);
                    } else if constexpr (std::is_same_v<
                                             Entry,
                                             TypeArgumentRationalRange>) {
                        target.appendString("rational_range");
                        appendRationalFingerprint(
                            target, entry.minimum);
                        appendRationalFingerprint(
                            target, entry.maximum);
                    } else if constexpr (std::is_same_v<
                                             Entry,
                                             TypeArgumentSetContains>) {
                        target.appendString("set_contains");
                        appendFingerprintRange(
                            target,
                            entry.required.values,
                            [](StableFingerprint64 &nested,
                               const EnumValueId &required) {
                                nested.appendString(
                                    required.value);
                            });
                    } else {
                        static_assert(
                            unsupportedFingerprintValue<Entry>);
                    }
                },
                predicate);
        });
}

void appendLogicalValueIdFingerprint(
    StableFingerprint64 &fingerprint,
    const LogicalValueId &value) {
    fingerprint.appendString(value.resource);
    fingerprint.appendUnsigned(value.version);
}

void appendLogicalGraphFingerprintPayload(
    StableFingerprint64 &fingerprint,
    const CompiledLogicalRenderGraph &graph) {
    fingerprint.appendString(graph.name);
    appendFingerprintRange(
        fingerprint, graph.resources,
        [](StableFingerprint64 &target,
           const LogicalResourceDesc &resource) {
            target.appendString(resource.name);
            appendLogicalTypeFingerprint(
                target, resource.type);
            target.appendString(
                logicalMaterializationRequirementName(
                    resource.materialization));
        });

    std::vector<const LogicalValueImport *> imports;
    imports.reserve(graph.imports.size());
    for (const auto &imported : graph.imports) {
        imports.push_back(&imported);
    }
    std::sort(
        imports.begin(), imports.end(),
        [](const auto *left, const auto *right) {
            if (left->value != right->value) {
                return left->value < right->value;
            }
            return logicalValueImportKindName(left->kind) <
                   logicalValueImportKindName(right->kind);
        });
    appendFingerprintRange(
        fingerprint, imports,
        [](StableFingerprint64 &target,
           const LogicalValueImport *imported) {
            appendLogicalValueIdFingerprint(
                target, imported->value);
            target.appendString(
                logicalValueImportKindName(
                    imported->kind));
        });

    appendFingerprintRange(
        fingerprint, graph.nodes,
        [](StableFingerprint64 &target,
           const LogicalGraphNode &node) {
            target.appendString(node.name);
            target.appendString(
                logicalGraphNodeKindName(node.kind));
            target.appendUnsigned(node.declaration_index);
            appendFingerprintRange(
                target, node.ports,
                [](StableFingerprint64 &nested,
                   const LogicalPortContract &port) {
                    nested.appendString(port.name);
                    nested.appendString(
                        logicalPortDirectionName(
                            port.direction));
                    appendLogicalTypePatternFingerprint(
                        nested, port.accepted_type);
                    appendFingerprintRange(
                        nested, port.relations,
                        [](StableFingerprint64 &relation_hash,
                           const LogicalPortRelation &relation) {
                            relation_hash.appendString(
                                logicalPortRelationKindName(
                                    relation.kind));
                            relation_hash.appendString(
                                relation.other_port);
                            appendRationalFingerprint(
                                relation_hash,
                                relation.scale_x);
                            appendRationalFingerprint(
                                relation_hash,
                                relation.scale_y);
                        });
                });
            appendFingerprintRange(
                target, node.uses,
                [](StableFingerprint64 &nested,
                   const LogicalResourceUse &use) {
                    nested.appendString(use.port);
                    nested.appendString(
                        logicalAccessModeName(use.access));
                    nested.appendString(
                        logicalAccessIntentName(use.intent));
                    nested.appendString(
                        logicalReadFootprintKindName(
                            use.footprint.kind));
                    appendFingerprintOptional(
                        nested, use.footprint.radius,
                        [](StableFingerprint64 &value_hash,
                           std::uint32_t radius) {
                            value_hash.appendUnsigned(radius);
                        });
                    appendFingerprintOptional(
                        nested, use.input_value,
                        appendLogicalValueIdFingerprint);
                    appendFingerprintOptional(
                        nested, use.output_value,
                        appendLogicalValueIdFingerprint);
                });
            appendFingerprintStrings(target, node.after);
            appendFingerprintStrings(target, node.before);
            appendFingerprintStrings(
                target, node.region_tags);
            const auto has_non_default_view_family =
                node.view_family != mainRenderViewFamilyId;
            appendFingerprintBoolean(
                target, has_non_default_view_family);
            if (has_non_default_view_family) {
                target.appendString(node.view_family);
            }
        });

    // data_edges are derived entirely from the node uses above and add no
    // independent identity. Avoid deriving and allocating them here.
    appendFingerprintOptional(
        fingerprint, graph.render_strategy,
        [](StableFingerprint64 &target,
           const RenderStrategySelection &selection) {
            target.appendString(selection.name);
            target.appendString(selection.provider);
            target.appendString(selection.implementation);
            target.appendString(selection.contract);
            target.appendString(selection.output_contract);
            target.appendString(selection.graph_variant);
            target.appendUnsigned(
                selection.facade_capability_bits);
            target.appendUnsigned(
                selection.input_config_fingerprint);
            target.appendUnsigned(
                selection.output_config_fingerprint);
            target.appendUnsigned(selection.provider_owner);
            target.appendUnsigned(selection.provider_identity);
            target.appendUnsigned(selection.provider_generation);
            target.appendUnsigned(selection.provider_version);
            target.appendUnsigned(
                selection.provider_capability_bits);
            appendFingerprintBoolean(
                target, selection.explicitly_selected);
        });
    appendFingerprintRange(
        fingerprint, graph.graph_transforms,
        [](StableFingerprint64 &target,
           const LogicalGraphTransformSelection &selection) {
            target.appendString(selection.name);
            target.appendString(selection.provider);
            target.appendString(selection.implementation);
            target.appendString(selection.contract);
            target.appendUnsigned(
                selection.boundary_fingerprint);
            target.appendUnsigned(
                selection.input_graph_fingerprint);
            target.appendUnsigned(
                selection.output_graph_fingerprint);
            target.appendUnsigned(selection.provider_owner);
            target.appendUnsigned(selection.provider_identity);
            target.appendUnsigned(selection.provider_generation);
            target.appendUnsigned(selection.provider_version);
            target.appendUnsigned(
                selection.provider_capability_bits);
            target.appendUnsigned(selection.transform_index);
            appendFingerprintBoolean(
                target, selection.explicitly_selected);
        });
    appendFingerprintRange(
        fingerprint, graph.subgraph_replacements,
        [](StableFingerprint64 &target,
           const LogicalSubgraphReplacementSelection &selection) {
            target.appendString(selection.region);
            target.appendString(selection.provider);
            target.appendString(selection.implementation);
            target.appendString(selection.contract);
            target.appendUnsigned(
                selection.contract_fingerprint);
            target.appendUnsigned(selection.provider_owner);
            target.appendUnsigned(selection.provider_identity);
            target.appendUnsigned(selection.provider_generation);
            target.appendUnsigned(selection.provider_version);
            target.appendUnsigned(
                selection.provider_capability_bits);
            appendFingerprintBoolean(
                target, selection.explicitly_selected);
            appendFingerprintStrings(
                target, selection.source_nodes);
            appendFingerprintStrings(
                target, selection.replacement_nodes);
        });
    appendFingerprintRange(
        fingerprint, graph.decisions,
        [](StableFingerprint64 &target,
           const LogicalCompileDecision &decision) {
            target.appendString(decision.code);
            target.appendString(decision.subject);
            target.appendString(decision.detail);
        });
}

std::uint64_t parseFingerprint(
    const nlohmann::json &value,
    std::string_view context) {
    if (!value.is_string()) {
        throw std::runtime_error(
            std::string{context} +
            " must be an fnv1a64 fingerprint string");
    }
    const auto &encoded =
        value.get_ref<const std::string &>();
    return parseStableFingerprint64(encoded, context);
}

void requireOnlyKeys(
    const nlohmann::json &object,
    std::initializer_list<std::string_view> allowed,
    std::string_view context) {
    for (auto field = object.begin();
         field != object.end(); ++field) {
        if (std::find(
                allowed.begin(), allowed.end(),
                field.key()) == allowed.end()) {
            throw std::runtime_error(
                std::string{context} +
                " has unknown key '" + field.key() + "'");
        }
    }
}

std::string requireJsonString(
    const nlohmann::json &object, std::string_view key,
    std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty()) {
        throw std::runtime_error(
            std::string{context} +
            " requires non-empty string " +
            std::string{key});
    }
    return found->get<std::string>();
}

void requireNonEmpty(std::string_view value, std::string_view subject) {
    if (value.empty()) {
        throw std::runtime_error(std::string{subject} +
                                 " must not be empty");
    }
}

void requireVersionedName(std::string_view value,
                          std::string_view subject) {
    requireNonEmpty(value, subject);
    try {
        const auto parsed = parseSemanticTypeId(value);
        if (semanticTypeIdName(parsed) != value) {
            throw std::runtime_error("name is not canonical");
        }
    } catch (const std::runtime_error &error) {
        throw std::runtime_error(
            std::string{subject} +
            " must use namespace.name@major: " + std::string{value} +
            " (" + error.what() + ")");
    }
}

void canonicalizeCapabilities(std::vector<std::string> &capabilities,
                              std::string_view subject) {
    for (const auto &capability : capabilities) {
        requireVersionedName(capability, subject);
    }
    std::sort(capabilities.begin(), capabilities.end());
    capabilities.erase(
        std::unique(capabilities.begin(), capabilities.end()),
        capabilities.end());
}

bool hasCapability(std::span<const std::string> capabilities,
                   std::string_view capability) {
    return std::binary_search(capabilities.begin(), capabilities.end(),
                              capability);
}

int footprintRank(LogicalReadFootprintKind footprint) {
    switch (footprint) {
    case LogicalReadFootprintKind::none: return 0;
    case LogicalReadFootprintKind::same_pixel: return 1;
    case LogicalReadFootprintKind::neighborhood: return 2;
    case LogicalReadFootprintKind::arbitrary: return 3;
    case LogicalReadFootprintKind::temporal: return 4;
    }
    throw std::runtime_error("unknown logical read footprint");
}

void widenFootprint(LogicalReadFootprintKind &destination,
                    LogicalReadFootprintKind source) {
    if (footprintRank(source) > footprintRank(destination)) {
        destination = source;
    }
}

template <typename Value>
void sortAndUnique(std::vector<Value> &values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

ResourcePattern canonicalizePattern(
    const LogicalTypeRegistry &types,
    const LogicalResourceDesc &resource,
    ResourcePattern pattern) {
    requireVersionedName(pattern.id, "resource pattern id");
    requireNonEmpty(pattern.provenance,
                    "resource pattern provenance");

    const auto match =
        matchLogicalType(types, resource.type, pattern.applicable_type);
    if (match.status == LogicalTypeMatchStatus::rejected ||
        match.status == LogicalTypeMatchStatus::convertible) {
        throw std::runtime_error(
            "resource pattern '" + pattern.id + "' does not apply to '" +
            resource.name + "': " + match.reason_code + " (" +
            match.detail + ")");
    }

    std::set<std::string, std::less<>> formats;
    for (auto &candidate : pattern.format_candidates) {
        requireNonEmpty(candidate.format,
                        "resource pattern format candidate");
        if (!formats.insert(candidate.format).second) {
            throw std::runtime_error(
                "resource pattern '" + pattern.id +
                "' has duplicate format candidate '" +
                candidate.format + "'");
        }
        canonicalizeCapabilities(
            candidate.required_capabilities,
            "resource pattern format capability");
    }
    if (resource.type.constructor == LogicalTypeConstructor::image &&
        pattern.format_candidates.empty()) {
        throw std::runtime_error(
            "image resource pattern '" + pattern.id +
            "' requires at least one format candidate");
    }
    return pattern;
}

struct CanonicalResourcePatternBinding {
    ResourcePattern pattern;
    std::optional<ResourceExtentPlan> extent;
    ImageMipLevelCount mip_levels;
    std::uint32_t array_layers = 1;
    ImageResourceDimension dimension =
        ImageResourceDimension::two_d;
};

ImageMipLevelCount canonicalizeMipLevels(
    const LogicalResourceDesc &resource,
    ImageMipLevelCount mip_levels) {
    if (resource.type.constructor !=
        LogicalTypeConstructor::image) {
        if (mip_levels != ImageMipLevelCount{}) {
            throw std::runtime_error(
                "resource mip-level binding applies to a non-image resource: " +
                resource.name);
        }
        return {};
    }
    if (mip_levels.mode ==
        ImageMipLevelMode::full_chain) {
        mip_levels.count = 1;
        return mip_levels;
    }
    if (mip_levels.count == 0) {
        throw std::runtime_error(
            "fixed resource mip-level count must be positive: " +
            resource.name);
    }
    return mip_levels;
}

std::uint32_t canonicalizeArrayLayers(
    const LogicalResourceDesc &resource,
    std::uint32_t array_layers) {
    if (resource.type.constructor !=
        LogicalTypeConstructor::image) {
        if (array_layers != 1) {
            throw std::runtime_error(
                "resource array-layer binding applies to a non-image resource: " +
                resource.name);
        }
        return 1;
    }
    if (array_layers == 0) {
        throw std::runtime_error(
            "resource array-layer count must be positive: " +
            resource.name);
    }
    return array_layers;
}

ImageResourceDimension canonicalizeDimension(
    const LogicalResourceDesc &resource,
    ImageResourceDimension dimension,
    std::uint32_t array_layers,
    const std::optional<ResourceExtentPlan> &extent) {
    if (resource.type.constructor !=
        LogicalTypeConstructor::image) {
        if (dimension !=
            ImageResourceDimension::two_d) {
            throw std::runtime_error(
                "resource image dimension applies to a non-image "
                "resource: " +
                resource.name);
        }
        return ImageResourceDimension::two_d;
    }
    if (dimension ==
        ImageResourceDimension::cube) {
        if (array_layers != 6) {
            throw std::runtime_error(
                "cube image resource requires exactly six array "
                "layers: " +
                resource.name);
        }
        if (extent &&
            extent->kind ==
                ResourceExtentKind::fixed &&
            extent->width != extent->height) {
            throw std::runtime_error(
                "cube image resource requires a square fixed "
                "extent: " +
                resource.name);
        }
    }
    return dimension;
}

ResourceExtentPlan canonicalizeExtent(
    const LogicalResourceDesc &resource,
    std::optional<ResourceExtentPlan> extent) {
    if (resource.type.constructor !=
        LogicalTypeConstructor::image) {
        if (extent) {
            throw std::runtime_error(
                "resource extent binding applies to a non-image resource: " +
                resource.name);
        }
        return {};
    }
    auto result = extent.value_or(ResourceExtentPlan{});
    switch (result.kind) {
    case ResourceExtentKind::output_relative:
        if (!std::isfinite(result.scale_x) ||
            !std::isfinite(result.scale_y) ||
            result.scale_x <= 0.0f ||
            result.scale_y <= 0.0f) {
            throw std::runtime_error(
                "output-relative resource extent requires finite positive scales: " +
                resource.name);
        }
        result.width = 0;
        result.height = 0;
        break;
    case ResourceExtentKind::fixed:
        if (result.width == 0 || result.height == 0) {
            throw std::runtime_error(
                "fixed resource extent requires non-zero dimensions: " +
                resource.name);
        }
        result.scale_x = 1.0f;
        result.scale_y = 1.0f;
        break;
    }
    return result;
}

std::map<std::string, CanonicalResourcePatternBinding,
         std::less<>>
canonicalPatternBindings(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &graph,
    std::span<const ResourcePatternBinding> bindings) {
    std::map<std::string, const LogicalResourceDesc *, std::less<>>
        resources;
    for (const auto &resource : graph.resources) {
        resources.emplace(resource.name, &resource);
    }

    std::map<std::string, CanonicalResourcePatternBinding,
             std::less<>>
        result;
    for (const auto &binding : bindings) {
        requireNonEmpty(binding.resource,
                        "resource pattern binding resource");
        const auto resource = resources.find(binding.resource);
        if (resource == resources.end()) {
            throw std::runtime_error(
                "resource pattern binding references unknown resource: " +
                binding.resource);
        }
        auto pattern =
            canonicalizePattern(types, *resource->second, binding.pattern);
        auto extent =
            resource->second->type.constructor ==
                    LogicalTypeConstructor::image
                ? std::optional<ResourceExtentPlan>{
                      canonicalizeExtent(
                          *resource->second, binding.extent)}
                : std::nullopt;
        auto mip_levels =
            canonicalizeMipLevels(
                *resource->second,
                binding.mip_levels);
        const auto array_layers =
            canonicalizeArrayLayers(
                *resource->second,
                binding.array_layers);
        const auto dimension =
            canonicalizeDimension(
                *resource->second,
                binding.dimension,
                array_layers, extent);
        if (!result
                 .emplace(
                     binding.resource,
                     CanonicalResourcePatternBinding{
                         .pattern = std::move(pattern),
                         .extent = std::move(extent),
                         .mip_levels = mip_levels,
                         .array_layers = array_layers,
                         .dimension = dimension,
                     })
                 .second) {
            throw std::runtime_error(
                "duplicate resource pattern binding: " +
                binding.resource);
        }
    }
    for (const auto &resource : graph.resources) {
        if (result.contains(resource.name)) continue;
        if (resource.type.constructor ==
                LogicalTypeConstructor::image ||
            resource.type.constructor ==
                LogicalTypeConstructor::buffer) {
            throw std::runtime_error(
                "logical resource has no ResourcePattern binding: " +
                resource.name);
        }
        result.emplace(
            resource.name,
            CanonicalResourcePatternBinding{
                .pattern =
                    ResourcePattern{
                        .id =
                            "pelican.render.compile_time_value_pattern@1",
                        .applicable_type =
                            exactLogicalTypePattern(types, resource.type),
                        .prefer_transient = false,
                        .allow_tile_local = false,
                        .allow_alias = false,
                        .provenance =
                            "builtin:compile-time-value-pattern-v1",
                    },
            });
    }
    return result;
}

std::vector<std::string> canonicalNodeOrder(
    const CompiledLogicalRenderGraph &graph,
    std::span<const std::string> requested_order) {
    std::vector<std::string> result;
    if (requested_order.empty()) {
        result =
            analyzeLogicalPlanningOpportunities(graph).node_order;
    } else {
        result.assign(requested_order.begin(), requested_order.end());
    }

    std::set<std::string, std::less<>> expected;
    for (const auto &node : graph.nodes) expected.insert(node.name);
    std::set<std::string, std::less<>> actual;
    for (const auto &node : result) {
        if (!actual.insert(node).second) {
            throw std::runtime_error(
                "target lowering node order contains duplicate node: " +
                node);
        }
    }
    if (actual != expected) {
        throw std::runtime_error(
            "target lowering node order is not a permutation of the "
            "canonical graph nodes");
    }
    return result;
}

TargetIrDialect expectedDialect(TargetLoweringStage stage) {
    switch (stage) {
    case TargetLoweringStage::canonical_workspace:
        return TargetIrDialect::logical;
    case TargetLoweringStage::target_execution_complete:
        return TargetIrDialect::execution_gpu;
    case TargetLoweringStage::vulkan_physical_complete:
        return TargetIrDialect::physical_vulkan;
    }
    throw std::runtime_error("unknown target lowering stage");
}

const TargetEndpoint &requireEndpoint(
    const TargetTopologySnapshot &topology, std::string_view id) {
    const auto *endpoint = findTargetEndpoint(topology, id);
    if (endpoint == nullptr) {
        throw std::runtime_error(
            "target planner endpoint does not exist: " +
            std::string{id});
    }
    if (endpoint->kind != TargetEndpointKind::vulkan_device) {
        throw std::runtime_error(
            "target planner endpoint is not a Vulkan device: " +
            std::string{id});
    }
    return *endpoint;
}

std::optional<std::uint32_t> endpointUnsignedFact(
    const TargetEndpoint &endpoint, std::string_view name) {
    const auto found = std::lower_bound(
        endpoint.facts.begin(), endpoint.facts.end(), name,
        [](const TargetFact &fact, std::string_view key) {
            return fact.name < key;
        });
    if (found == endpoint.facts.end() || found->name != name) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto *begin = found->value.data();
    const auto *end = begin + found->value.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::runtime_error(
            "target endpoint fact '" + found->name +
            "' must be an unsigned integer on endpoint '" +
            endpoint.id + "'");
    }
    return value;
}

VulkanPhysicalScopeKind physicalScopeKind(
    LogicalGraphNodeKind kind) {
    switch (kind) {
    case LogicalGraphNodeKind::render:
        return VulkanPhysicalScopeKind::rendering;
    case LogicalGraphNodeKind::compute:
        return VulkanPhysicalScopeKind::compute;
    case LogicalGraphNodeKind::snapshot_copy:
        return VulkanPhysicalScopeKind::transfer;
    case LogicalGraphNodeKind::output_transform:
        return VulkanPhysicalScopeKind::output;
    case LogicalGraphNodeKind::anchor:
        return VulkanPhysicalScopeKind::marker;
    }
    throw std::runtime_error("unknown logical graph node kind");
}

bool isMaterialized(VulkanResourceRepresentation representation) {
    return representation ==
               VulkanResourceRepresentation::materialized_image ||
           representation ==
               VulkanResourceRepresentation::materialized_buffer ||
           representation == VulkanResourceRepresentation::external;
}

struct SelectedFormat {
    std::string format;
    std::vector<std::string> required_capabilities;
};

SelectedFormat selectFormat(
    const ResourcePattern &pattern,
    const TargetEndpoint &endpoint,
    LogicalTypeConstructor constructor) {
    if (constructor != LogicalTypeConstructor::image) return {};
    const auto supported = std::find_if(
        pattern.format_candidates.begin(),
        pattern.format_candidates.end(),
        [&](const ResourceFormatCandidate &candidate) {
            return std::all_of(
                candidate.required_capabilities.begin(),
                candidate.required_capabilities.end(),
                [&](const std::string &capability) {
                    return hasCapability(endpoint.capabilities,
                                         capability);
                });
        });
    const auto &selected =
        supported == pattern.format_candidates.end()
            ? pattern.format_candidates.front()
            : *supported;
    return {selected.format, selected.required_capabilities};
}

std::string materializationReason(
    const TargetLoweringResource &resource, bool tile_candidate,
    bool transient_candidate,
    bool tile_local_eligible,
    VulkanResourceRepresentation representation) {
    if (representation == VulkanResourceRepresentation::external) {
        return "logical resource is externally owned";
    }
    if (resource.uses.produced_by_snapshot) {
        return "snapshot output requires a stable sampled image";
    }
    if (resource.logical.materialization ==
        LogicalMaterializationRequirement::required) {
        return "logical materialization requirement is required";
    }
    if (resource.pattern.require_store) {
        return "ResourcePattern requires contents to be stored";
    }
    if (resource.uses.widest_read ==
        LogicalReadFootprintKind::neighborhood) {
        return "neighborhood read cannot use same-pixel tile-local access";
    }
    if (resource.uses.widest_read ==
        LogicalReadFootprintKind::arbitrary) {
        return "arbitrary read requires a materialized resource";
    }
    if (resource.uses.widest_read ==
        LogicalReadFootprintKind::temporal) {
        return "temporal read requires persistent materialization";
    }
    if (representation ==
        VulkanResourceRepresentation::tile_local_attachment) {
        return "same-pixel render use selected tile-local access";
    }
    if (representation ==
        VulkanResourceRepresentation::transient_attachment) {
        return "write-only frame-local resource selected transient "
               "attachment";
    }
    if (tile_candidate && !tile_local_eligible) {
        return "tile-local constraints were not satisfied; materialized "
               "fallback selected";
    }
    if (transient_candidate &&
        resource.pattern.prefer_transient) {
        return "transient attachment constraints were not satisfied; "
               "materialized fallback selected";
    }
    return "desktop materialized sampled representation selected";
}

struct CandidateDraft {
    std::string name;
    std::vector<VulkanPhysicalResourcePlan> resources;
    std::vector<VulkanPhysicalScopePlan> scopes;
    std::vector<std::string> required_features;
    std::vector<PlanningDecision> decisions;
    std::vector<PlanningDiagnostic> diagnostics;
    std::vector<BackendConstraintFailure> failures;
    BackendCostEstimate cost;
};

struct ExternalDepthExportSelection {
    std::string source_resource;
    std::string reason;
};

std::optional<ExternalDepthExportSelection>
selectExternalDepthExport(
    const LogicalTypeRegistry &types,
    const TargetLoweringGraph &workspace,
    const std::optional<VulkanExternalDepthExportRequest>
        &request) {
    if (!request) return std::nullopt;

    const auto device_depth = deviceDepthV1(types);
    const std::optional<
        std::set<std::string, std::less<>>>
        compatible =
            request->compatible_source_resources
                ? std::optional{
                      std::set<std::string,
                               std::less<>>{
                          request
                              ->compatible_source_resources
                              ->begin(),
                          request
                              ->compatible_source_resources
                              ->end()}}
                : std::nullopt;
    const auto is_compatible =
        [&](std::string_view resource) {
            return !compatible ||
                   compatible->contains(resource);
        };
    std::map<std::string, const LogicalResourceDesc *,
             std::less<>>
        resources;
    for (const auto &resource : workspace.resources) {
        resources.emplace(resource.logical.name,
                          &resource.logical);
    }

    struct Candidate {
        std::string resource;
        std::size_t latest_write = 0;
        bool camera_surface = false;
    };
    std::map<std::string, Candidate, std::less<>>
        candidates;
    for (std::size_t node_index = 0;
         node_index < workspace.nodes.size(); ++node_index) {
        const auto &node =
            workspace.nodes[node_index].logical;
        bool writes_non_depth_image = false;
        for (const auto &use : node.uses) {
            if (!use.output_value) continue;
            const auto found = resources.find(
                use.output_value->resource);
            if (found == resources.end()) continue;
            if (found->second->type.constructor ==
                    LogicalTypeConstructor::image &&
                found->second->type != device_depth) {
                writes_non_depth_image = true;
            }
        }
        for (const auto &use : node.uses) {
            if (!use.output_value) continue;
            const auto found = resources.find(
                use.output_value->resource);
            if (found == resources.end() ||
                found->second->type != device_depth ||
                !is_compatible(
                    use.output_value->resource)) {
                continue;
            }
            auto [candidate, inserted] =
                candidates.try_emplace(
                    use.output_value->resource,
                    Candidate{
                        .resource =
                            use.output_value->resource,
                    });
            (void)inserted;
            candidate->second.latest_write =
                node_index;
            candidate->second.camera_surface =
                candidate->second.camera_surface ||
                writes_non_depth_image;
        }
    }

    if (request->source_resource) {
        const auto found =
            resources.find(*request->source_resource);
        if (found == resources.end()) {
            throw std::runtime_error(
                "external depth export names an unknown logical "
                "resource: " +
                *request->source_resource);
        }
        if (found->second->type != device_depth) {
            throw std::runtime_error(
                "external depth export source must have typed "
                "device/projection depth semantics: " +
                *request->source_resource);
        }
        if (!is_compatible(
                *request->source_resource)) {
            throw std::runtime_error(
                "external depth export source does not support "
                "the required physical transfer operation: " +
                *request->source_resource);
        }
        if (!candidates.contains(
                *request->source_resource)) {
            throw std::runtime_error(
                "external depth export source is never written by "
                "the logical graph: " +
                *request->source_resource);
        }
        return ExternalDepthExportSelection{
            *request->source_resource,
            "explicit external depth source selected by target "
            "policy",
        };
    }

    const Candidate *selected = nullptr;
    const auto prefer = [&](const Candidate &candidate) {
        if (selected == nullptr ||
            candidate.camera_surface >
                selected->camera_surface ||
            (candidate.camera_surface ==
                 selected->camera_surface &&
             candidate.latest_write >
                 selected->latest_write) ||
            (candidate.camera_surface ==
                 selected->camera_surface &&
             candidate.latest_write ==
                 selected->latest_write &&
             candidate.resource <
                 selected->resource)) {
            selected = &candidate;
        }
    };
    for (const auto &[name, candidate] : candidates) {
        (void)name;
        prefer(candidate);
    }
    if (selected == nullptr) {
        if (request->required) {
            throw std::runtime_error(
                "required external depth export found no written "
                "compatible device/projection depth resource");
        }
        return std::nullopt;
    }
    return ExternalDepthExportSelection{
        selected->resource,
        selected->camera_surface
            ? "inferred latest typed depth associated with a "
              "camera color surface"
            : "inferred latest written typed device depth",
    };
}

class DisjointSet {
    std::vector<std::size_t> parents_;

  public:
    explicit DisjointSet(std::size_t size) : parents_(size) {
        std::iota(parents_.begin(), parents_.end(), 0);
    }

    std::size_t find(std::size_t value) {
        if (parents_[value] != value) {
            parents_[value] = find(parents_[value]);
        }
        return parents_[value];
    }

    void join(std::size_t left, std::size_t right) {
        left = find(left);
        right = find(right);
        if (left != right) parents_[right] = left;
    }
};

const VulkanPhysicalResourcePlan *findPhysicalResource(
    std::span<const VulkanPhysicalResourcePlan> resources,
    std::string_view name) {
    const auto found = std::lower_bound(
        resources.begin(), resources.end(), name,
        [](const VulkanPhysicalResourcePlan &resource,
           std::string_view key) {
            return resource.logical_resource < key;
        });
    return found != resources.end() &&
                   found->logical_resource == name
               ? &*found
               : nullptr;
}

std::vector<std::string> attachmentOutputs(
    const LogicalGraphNode &node,
    const std::map<std::string, const LogicalResourceDesc *,
                   std::less<>> &resources) {
    if (node.kind != LogicalGraphNodeKind::render) return {};
    std::vector<std::string> result;
    for (const auto &use : node.uses) {
        if (!use.output_value ||
            (use.intent != LogicalAccessIntent::automatic &&
             use.intent != LogicalAccessIntent::attachment)) {
            continue;
        }
        const auto resource =
            resources.find(use.output_value->resource);
        if (resource == resources.end() ||
            resource->second->type.constructor !=
                LogicalTypeConstructor::image) {
            continue;
        }
        result.push_back(use.output_value->resource);
    }
    sortAndUnique(result);
    return result;
}

ResolvedSampleCountPlan resolveTargetSampleCounts(
    const CompiledLogicalRenderGraph &graph,
    const VulkanSampleCountPlanRequest &request) {
    std::map<std::string, const LogicalResourceDesc *, std::less<>>
        logical_resources;
    for (const auto &resource : graph.resources) {
        logical_resources.emplace(resource.name, &resource);
    }

    std::set<std::string, std::less<>> attachment_names;
    std::map<std::string, std::vector<std::string>, std::less<>>
        node_attachments;
    std::map<std::string, const LogicalGraphNode *, std::less<>>
        nodes;
    for (const auto &node : graph.nodes) {
        if (!nodes.emplace(node.name, &node).second) {
            throw std::runtime_error(
                "sample-count planning found duplicate logical node: " +
                node.name);
        }
        auto attachments =
            attachmentOutputs(node, logical_resources);
        attachment_names.insert(attachments.begin(),
                                attachments.end());
        node_attachments.emplace(node.name,
                                 std::move(attachments));
    }

    std::set<std::string, std::less<>> geometry_nodes;
    for (const auto &name : request.geometry_nodes) {
        const auto node = nodes.find(name);
        if (node == nodes.end() ||
            node->second->kind != LogicalGraphNodeKind::render) {
            throw std::runtime_error(
                "sample-count geometry node is not a logical render node: " +
                name);
        }
        if (!geometry_nodes.insert(name).second) {
            throw std::runtime_error(
                "sample-count geometry nodes must be unique: " + name);
        }
    }

    std::vector<std::string> attachments(
        attachment_names.begin(), attachment_names.end());
    std::map<std::string, std::size_t, std::less<>>
        attachment_indices;
    for (std::size_t index = 0; index < attachments.size(); ++index) {
        attachment_indices.emplace(attachments[index], index);
    }

    DisjointSet sets{attachments.size()};
    std::vector<bool> geometry_members(attachments.size(), false);
    for (const auto &[node_name, outputs] : node_attachments) {
        if (outputs.empty()) continue;
        const auto first = attachment_indices.at(outputs.front());
        for (std::size_t index = 1; index < outputs.size(); ++index) {
            sets.join(first, attachment_indices.at(outputs[index]));
        }
        if (geometry_nodes.contains(node_name)) {
            for (const auto &output : outputs) {
                geometry_members[attachment_indices.at(output)] = true;
            }
        }
    }

    std::map<std::string, SampleCountResourceCapability, std::less<>>
        capabilities;
    for (const auto &capability : request.capabilities) {
        if (!attachment_indices.contains(capability.resource)) {
            throw std::runtime_error(
                "sample-count capability is not an attachment logical "
                "resource: " +
                capability.resource);
        }
        if (!capabilities.emplace(capability.resource, capability).second) {
            throw std::runtime_error(
                "sample-count capabilities must name unique resources: " +
                capability.resource);
        }
    }
    for (const auto &attachment : attachments) {
        if (!capabilities.contains(attachment)) {
            throw std::runtime_error(
                "sample-count attachment lacks a physical capability: " +
                attachment);
        }
    }

    std::set<std::size_t> explicit_members;
    for (const auto &target : request.policy.targets) {
        const auto found = attachment_indices.find(target);
        if (found == attachment_indices.end()) {
            throw std::runtime_error(
                "multisampling target is not an attachment logical "
                "resource: " +
                target);
        }
        explicit_members.insert(found->second);
    }

    std::map<std::size_t, std::vector<std::size_t>> components;
    for (std::size_t index = 0; index < attachments.size(); ++index) {
        components[sets.find(index)].push_back(index);
    }

    std::vector<SampleCountGroupRequest> groups;
    groups.reserve(components.size());
    for (const auto &[root, members] : components) {
        (void)root;
        bool selected =
            request.policy.scope == SampleCountScope::all;
        std::vector<SampleCountResourceCapability> resources;
        resources.reserve(members.size());
        for (const auto member : members) {
            if (request.policy.scope == SampleCountScope::geometry &&
                geometry_members[member]) {
                selected = true;
            }
            if (explicit_members.contains(member)) selected = true;
            resources.push_back(capabilities.at(attachments[member]));
        }
        std::sort(
            resources.begin(), resources.end(),
            [](const auto &left, const auto &right) {
                return left.resource < right.resource;
            });
        groups.push_back(SampleCountGroupRequest{
            .id = "attachments:" + resources.front().resource,
            .multisampling_enabled = selected,
            .resources = std::move(resources),
        });
    }
    return resolveSampleCountPlan(request.policy.request, groups);
}

void applyResolvedSampleCounts(
    CandidateDraft &candidate,
    const ResolvedSampleCountPlan &sample_count_plan) {
    for (const auto &resolved : sample_count_plan.resources) {
        const auto found = std::lower_bound(
            candidate.resources.begin(), candidate.resources.end(),
            resolved.resource,
            [](const VulkanPhysicalResourcePlan &resource,
               std::string_view name) {
                return resource.logical_resource < name;
            });
        if (found == candidate.resources.end() ||
            found->logical_resource != resolved.resource) {
            throw std::runtime_error(
                "sample-count physical resource was not lowered: " +
                resolved.resource);
        }
        if (found->format != resolved.format) {
            throw std::runtime_error(
                "sample-count format does not match lowered physical "
                "format for '" +
                resolved.resource + "': capability reports '" +
                resolved.format + "', lowering selected '" +
                found->format + "'");
        }
        found->rasterization_samples = resolved.samples;
        found->resolve_required =
            resolved.samples > 1 &&
            found->representation !=
                VulkanResourceRepresentation::external;
        // Runtime aliasing currently owns one resolved single-sample image
        // per logical resource. A multisample target also needs a distinct
        // attachment allocation, so keep it out of alias planning until both
        // surfaces can be lowered as one physical contract.
        if (resolved.samples > 1) {
            found->aliasable = false;
        }
    }
}

bool resolvesToSingleSample(
    std::string_view resource,
    const ResolvedSampleCountPlan *sample_count_plan) {
    if (sample_count_plan == nullptr) {
        return true;
    }
    const auto found = std::find_if(
        sample_count_plan->resources.begin(),
        sample_count_plan->resources.end(),
        [&](const auto &resolved) {
            return resolved.resource == resource;
        });
    return found == sample_count_plan->resources.end() ||
           found->samples == 1;
}

std::uint32_t nodeRasterizationSamples(
    const LogicalGraphNode &node,
    const std::map<std::string, const LogicalResourceDesc *,
                   std::less<>> &logical_resources,
    std::span<const VulkanPhysicalResourcePlan> resources) {
    std::optional<std::uint32_t> result;
    for (const auto &name :
         attachmentOutputs(node, logical_resources)) {
        const auto *resource =
            findPhysicalResource(resources, name);
        if (resource == nullptr) continue;
        if (result && *result != resource->rasterization_samples) {
            throw std::runtime_error(
                "logical render node '" + node.name +
                "' has incompatible attachment sample counts");
        }
        result = resource->rasterization_samples;
    }
    return result.value_or(1);
}

const LogicalResourceUse *findNodeUse(
    const LogicalGraphNode &node, std::string_view port) {
    const auto found = std::find_if(
        node.uses.begin(), node.uses.end(),
        [&](const LogicalResourceUse &use) {
            return use.port == port;
        });
    return found == node.uses.end() ? nullptr : &*found;
}

std::map<std::string, PlanningNodeConstraint, std::less<>>
nodeConstraintMap(
    std::span<const PlanningNodeConstraint> constraints) {
    std::map<std::string, PlanningNodeConstraint, std::less<>> result;
    for (const auto &constraint : constraints) {
        result.emplace(constraint.node, constraint);
    }
    return result;
}

bool constraintBlocksFusion(
    const std::map<std::string, PlanningNodeConstraint, std::less<>>
        &constraints,
    std::string_view node) {
    const auto found = constraints.find(node);
    return found != constraints.end() &&
           (found->second.serial || found->second.isolate);
}

} // namespace

std::string_view resourcePatternFallbackName(
    ResourcePatternFallback fallback) {
    switch (fallback) {
    case ResourcePatternFallback::materialize: return "materialize";
    case ResourcePatternFallback::reject: return "reject";
    }
    throw std::runtime_error("unknown ResourcePattern fallback");
}

std::string_view resourceExtentKindName(
    ResourceExtentKind kind) {
    switch (kind) {
    case ResourceExtentKind::output_relative:
        return "output_relative";
    case ResourceExtentKind::fixed:
        return "fixed";
    }
    throw std::runtime_error("unknown resource extent kind");
}

std::string_view targetIrDialectName(TargetIrDialect dialect) {
    switch (dialect) {
    case TargetIrDialect::logical: return "logical";
    case TargetIrDialect::execution_gpu: return "execution.gpu";
    case TargetIrDialect::physical_vulkan:
        return "physical.vulkan";
    }
    throw std::runtime_error("unknown target IR dialect");
}

std::string_view targetLoweringStageName(TargetLoweringStage stage) {
    switch (stage) {
    case TargetLoweringStage::canonical_workspace:
        return "canonical_workspace";
    case TargetLoweringStage::target_execution_complete:
        return "target_execution_complete";
    case TargetLoweringStage::vulkan_physical_complete:
        return "vulkan_physical_complete";
    }
    throw std::runtime_error("unknown target lowering stage");
}

TargetLoweringGraph makeTargetLoweringGraph(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &canonical_graph,
    std::span<const ResourcePatternBinding> pattern_bindings,
    std::span<const std::string> requested_node_order) {
    validateCompiledLogicalRenderGraph(types, canonical_graph);
    const auto patterns = canonicalPatternBindings(
        types, canonical_graph, pattern_bindings);
    const auto node_order =
        canonicalNodeOrder(canonical_graph, requested_node_order);

    std::map<std::string, const LogicalGraphNode *, std::less<>>
        logical_nodes;
    for (const auto &node : canonical_graph.nodes) {
        logical_nodes.emplace(node.name, &node);
    }

    TargetLoweringGraph result;
    result.name = canonical_graph.name;
    result.nodes.reserve(node_order.size());
    for (const auto &name : node_order) {
        const auto &node = *logical_nodes.at(name);
        result.nodes.push_back(TargetLoweringNode{
            .logical = node,
            .dialect = TargetIrDialect::logical,
            .source_nodes = {node.name},
        });
    }

    auto resources = canonical_graph.resources;
    std::sort(resources.begin(), resources.end(),
              [](const LogicalResourceDesc &left,
                 const LogicalResourceDesc &right) {
                  return left.name < right.name;
              });
    result.resources.reserve(resources.size());
    for (auto &resource : resources) {
        const auto pattern = patterns.find(resource.name);
        result.resources.push_back(TargetLoweringResource{
            .logical = std::move(resource),
            .pattern = pattern->second.pattern,
            .extent = pattern->second.extent,
            .mip_levels =
                pattern->second.mip_levels,
            .array_layers =
                pattern->second.array_layers,
            .dimension =
                pattern->second.dimension,
        });
    }

    std::map<std::string, TargetLoweringResource *, std::less<>>
        lowering_resources;
    for (auto &resource : result.resources) {
        lowering_resources.emplace(resource.logical.name, &resource);
    }
    for (std::size_t node_index = 0;
         node_index < result.nodes.size(); ++node_index) {
        const auto &node = result.nodes[node_index].logical;
        for (const auto &use : node.uses) {
            const auto &value =
                use.input_value ? *use.input_value : *use.output_value;
            auto &resource =
                *lowering_resources.at(value.resource);
            if (!resource.lifetime.used) {
                resource.lifetime = {true, node_index, node_index};
            } else {
                resource.lifetime.first_use =
                    std::min(resource.lifetime.first_use, node_index);
                resource.lifetime.last_use =
                    std::max(resource.lifetime.last_use, node_index);
            }

            const auto reads =
                use.access == LogicalAccessMode::read ||
                use.access == LogicalAccessMode::read_write;
            const auto writes =
                use.access == LogicalAccessMode::write ||
                use.access == LogicalAccessMode::read_write;
            resource.uses.read = resource.uses.read || reads;
            resource.uses.written = resource.uses.written || writes;
            if (reads) {
                widenFootprint(resource.uses.widest_read,
                               use.footprint.kind);
            }
            if (node.kind != LogicalGraphNodeKind::render) {
                resource.uses.non_render_access = true;
            }
            switch (use.intent) {
            case LogicalAccessIntent::automatic:
                if (node.kind == LogicalGraphNodeKind::render) {
                    resource.uses.attachment_access = true;
                }
                break;
            case LogicalAccessIntent::sampled:
                resource.uses.sampled_access = true;
                break;
            case LogicalAccessIntent::attachment:
                resource.uses.attachment_access = true;
                break;
            case LogicalAccessIntent::storage:
                resource.uses.storage_access = true;
                break;
            case LogicalAccessIntent::transfer:
                resource.uses.transfer_access = true;
                break;
            case LogicalAccessIntent::host:
                resource.uses.host_access = true;
                break;
            }
            if (writes &&
                node.kind == LogicalGraphNodeKind::snapshot_copy) {
                resource.uses.produced_by_snapshot = true;
            }
        }
    }

    if (!result.nodes.empty()) {
        const auto terminal = result.nodes.size() - 1;
        for (auto &resource : result.resources) {
            if (!resource.lifetime.used) continue;
            if (resource.logical.materialization ==
                    LogicalMaterializationRequirement::required ||
                resource.logical.materialization ==
                    LogicalMaterializationRequirement::external ||
                resource.pattern.require_store) {
                resource.lifetime.last_use = terminal;
            }
        }
    }

    for (const auto &resource : result.resources) {
        result.decisions.push_back(PlanningDecision{
            "pelican.plan.resource_pattern_bound@1",
            resource.logical.name,
            resource.pattern.id,
            resource.pattern.provenance,
        });
    }
    validateTargetLoweringGraphDialect(
        result, TargetLoweringStage::canonical_workspace);
    return result;
}

void validateTargetLoweringGraphDialect(
    const TargetLoweringGraph &graph,
    TargetLoweringStage expected_stage) {
    requireNonEmpty(graph.name, "target lowering graph name");
    if (graph.stage != expected_stage) {
        throw std::runtime_error(
            "target lowering graph stage mismatch: expected " +
            std::string{targetLoweringStageName(expected_stage)} +
            ", got " +
            std::string{targetLoweringStageName(graph.stage)});
    }
    const auto expected = expectedDialect(expected_stage);
    std::set<std::string, std::less<>> nodes;
    for (const auto &node : graph.nodes) {
        requireNonEmpty(node.logical.name,
                        "target lowering node name");
        if (!nodes.insert(node.logical.name).second) {
            throw std::runtime_error(
                "duplicate target lowering node: " +
                node.logical.name);
        }
        if (node.dialect != expected) {
            throw std::runtime_error(
                "target lowering dialect is illegal at stage '" +
                std::string{targetLoweringStageName(expected_stage)} +
                "': node '" + node.logical.name + "' is '" +
                std::string{targetIrDialectName(node.dialect)} +
                "', expected '" +
                std::string{targetIrDialectName(expected)} + "'");
        }
        if (node.source_nodes.empty()) {
            throw std::runtime_error(
                "target lowering node lacks canonical provenance: " +
                node.logical.name);
        }
    }
    std::set<std::string, std::less<>> resources;
    for (const auto &resource : graph.resources) {
        requireNonEmpty(resource.logical.name,
                        "target lowering resource name");
        if (!resources.insert(resource.logical.name).second) {
            throw std::runtime_error(
                "duplicate target lowering resource: " +
                resource.logical.name);
        }
        if (resource.dialect != expected) {
            throw std::runtime_error(
                "target lowering dialect is illegal at stage '" +
                std::string{targetLoweringStageName(expected_stage)} +
                "': resource '" + resource.logical.name + "' is '" +
                std::string{targetIrDialectName(resource.dialect)} +
                "', expected '" +
                std::string{targetIrDialectName(expected)} + "'");
        }
    }
}

void lowerTargetExecutionDialect(TargetLoweringGraph &graph) {
    validateTargetLoweringGraphDialect(
        graph, TargetLoweringStage::canonical_workspace);
    for (auto &node : graph.nodes) {
        node.dialect = TargetIrDialect::execution_gpu;
    }
    for (auto &resource : graph.resources) {
        resource.dialect = TargetIrDialect::execution_gpu;
    }
    graph.stage = TargetLoweringStage::target_execution_complete;
    graph.decisions.push_back(PlanningDecision{
        "pelican.plan.dialect_lowered@1", graph.name,
        "execution.gpu", "logical operations were eliminated from the "
                         "disposable target workspace",
    });
    validateTargetLoweringGraphDialect(
        graph, TargetLoweringStage::target_execution_complete);
}

void lowerVulkanPhysicalDialect(TargetLoweringGraph &graph) {
    validateTargetLoweringGraphDialect(
        graph, TargetLoweringStage::target_execution_complete);
    for (auto &node : graph.nodes) {
        node.dialect = TargetIrDialect::physical_vulkan;
    }
    for (auto &resource : graph.resources) {
        resource.dialect = TargetIrDialect::physical_vulkan;
    }
    graph.stage = TargetLoweringStage::vulkan_physical_complete;
    graph.decisions.push_back(PlanningDecision{
        "pelican.plan.dialect_lowered@1", graph.name,
        "physical.vulkan",
        "execution.gpu operations were eliminated after Vulkan lowering",
    });
    validateTargetLoweringGraphDialect(
        graph, TargetLoweringStage::vulkan_physical_complete);
}

nlohmann::ordered_json targetLoweringGraphToJson(
    const TargetLoweringGraph &graph) {
    validateTargetLoweringGraphDialect(graph, graph.stage);
    nlohmann::ordered_json result{
        {"schema", "pelican.target_lowering_graph"},
        {"version", 1},
        {"graph", graph.name},
        {"stage", targetLoweringStageName(graph.stage)},
    };
    result["nodes"] = nlohmann::ordered_json::array();
    for (const auto &node : graph.nodes) {
        result["nodes"].push_back(nlohmann::ordered_json{
            {"name", node.logical.name},
            {"kind", logicalGraphNodeKindName(node.logical.kind)},
            {"dialect", targetIrDialectName(node.dialect)},
            {"sources", node.source_nodes},
            {"regions", node.logical.region_tags},
            {"required_physical_features",
             node.required_physical_features},
        });
    }
    result["resources"] = nlohmann::ordered_json::array();
    for (const auto &resource : graph.resources) {
        nlohmann::ordered_json lifetime{
            {"used", resource.lifetime.used}};
        if (resource.lifetime.used) {
            lifetime["first_use"] = resource.lifetime.first_use;
            lifetime["last_use"] = resource.lifetime.last_use;
        }
        nlohmann::ordered_json formats =
            nlohmann::ordered_json::array();
        for (const auto &candidate :
             resource.pattern.format_candidates) {
            formats.push_back(nlohmann::ordered_json{
                {"format", candidate.format},
                {"required_capabilities",
                 candidate.required_capabilities},
            });
        }
        result["resources"].push_back(nlohmann::ordered_json{
            {"name", resource.logical.name},
            {"dialect", targetIrDialectName(resource.dialect)},
            {"type", logicalTypeToJson(resource.logical.type)},
            {"logical_materialization",
             logicalMaterializationRequirementName(
                 resource.logical.materialization)},
            {"pattern",
             nlohmann::ordered_json{
                 {"id", resource.pattern.id},
                 {"formats", std::move(formats)},
                 {"prefer_transient",
                  resource.pattern.prefer_transient},
                 {"allow_tile_local",
                  resource.pattern.allow_tile_local},
                 {"allow_alias", resource.pattern.allow_alias},
                 {"require_store", resource.pattern.require_store},
                 {"local_read_fallback",
                  resourcePatternFallbackName(
                      resource.pattern.local_read_fallback)},
                 {"estimated_bytes",
                  resource.pattern.estimated_bytes},
                 {"provenance", resource.pattern.provenance},
             }},
            {"extent",
             resource.extent
                 ? nlohmann::ordered_json{
                       {"kind",
                        resourceExtentKindName(
                            resource.extent->kind)},
                       {"scale_x", resource.extent->scale_x},
                       {"scale_y", resource.extent->scale_y},
                       {"width", resource.extent->width},
                       {"height", resource.extent->height},
                   }
                 : nlohmann::ordered_json(nullptr)},
            {"mip_levels",
             nlohmann::ordered_json{
                 {"mode",
                  imageMipLevelModeName(
                      resource.mip_levels.mode)},
                 {"count",
                 resource.mip_levels.count},
             }},
            {"array_layers",
             resource.array_layers},
            {"dimension",
             imageResourceDimensionName(
                 resource.dimension)},
            {"uses",
             nlohmann::ordered_json{
                 {"read", resource.uses.read},
                 {"written", resource.uses.written},
                 {"attachment_access",
                  resource.uses.attachment_access},
                 {"sampled_access",
                  resource.uses.sampled_access},
                 {"storage_access",
                  resource.uses.storage_access},
                 {"transfer_access",
                  resource.uses.transfer_access},
                 {"host_access", resource.uses.host_access},
                 {"non_render_access",
                  resource.uses.non_render_access},
                 {"produced_by_snapshot",
                  resource.uses.produced_by_snapshot},
                 {"widest_read",
                  logicalReadFootprintKindName(
                      resource.uses.widest_read)},
             }},
            {"lifetime", std::move(lifetime)},
            {"required_physical_features",
             resource.required_physical_features},
        });
    }
    result["decisions"] = nlohmann::ordered_json::array();
    for (const auto &decision : graph.decisions) {
        result["decisions"].push_back(nlohmann::ordered_json{
            {"id", decision.id},
            {"subject", decision.subject},
            {"selected", decision.selected},
            {"detail", decision.detail},
        });
    }
    return result;
}

std::string_view vulkanResourceRepresentationName(
    VulkanResourceRepresentation representation) {
    switch (representation) {
    case VulkanResourceRepresentation::materialized_image:
        return "materialized_image";
    case VulkanResourceRepresentation::materialized_buffer:
        return "materialized_buffer";
    case VulkanResourceRepresentation::transient_attachment:
        return "transient_attachment";
    case VulkanResourceRepresentation::tile_local_attachment:
        return "tile_local_attachment";
    case VulkanResourceRepresentation::external:
        return "external";
    }
    throw std::runtime_error(
        "unknown Vulkan resource representation");
}

std::string_view vulkanResourceViewLayoutName(
    VulkanResourceViewLayout layout) {
    switch (layout) {
    case VulkanResourceViewLayout::shared_2d:
        return "shared_2d";
    case VulkanResourceViewLayout::sequential_2d:
        return "sequential_2d";
    case VulkanResourceViewLayout::layered_2d_array:
        return "layered_2d_array";
    case VulkanResourceViewLayout::family_2d_array:
        return "family_2d_array";
    }
    throw std::runtime_error(
        "unknown Vulkan resource view layout");
}

std::string_view vulkanPhysicalScopeKindName(
    VulkanPhysicalScopeKind kind) {
    switch (kind) {
    case VulkanPhysicalScopeKind::rendering: return "rendering";
    case VulkanPhysicalScopeKind::compute: return "compute";
    case VulkanPhysicalScopeKind::transfer: return "transfer";
    case VulkanPhysicalScopeKind::output: return "output";
    case VulkanPhysicalScopeKind::marker: return "marker";
    }
    throw std::runtime_error("unknown Vulkan physical scope kind");
}

std::string_view vulkanPhysicalAttachmentAspectName(
    VulkanPhysicalAttachmentAspect aspect) {
    switch (aspect) {
    case VulkanPhysicalAttachmentAspect::color: return "color";
    case VulkanPhysicalAttachmentAspect::depth: return "depth";
    }
    throw std::runtime_error(
        "unknown Vulkan physical attachment aspect");
}

std::string_view vulkanPhysicalAttachmentLoadOpName(
    VulkanPhysicalAttachmentLoadOp op) {
    switch (op) {
    case VulkanPhysicalAttachmentLoadOp::load: return "load";
    case VulkanPhysicalAttachmentLoadOp::clear: return "clear";
    case VulkanPhysicalAttachmentLoadOp::discard: return "discard";
    }
    throw std::runtime_error(
        "unknown Vulkan physical attachment load op");
}

std::string_view vulkanPhysicalAttachmentStoreOpName(
    VulkanPhysicalAttachmentStoreOp op) {
    switch (op) {
    case VulkanPhysicalAttachmentStoreOp::store: return "store";
    case VulkanPhysicalAttachmentStoreOp::discard: return "discard";
    }
    throw std::runtime_error(
        "unknown Vulkan physical attachment store op");
}

namespace {

bool canAppendMaterializedRenderingScope(
    const VulkanPhysicalScopePlan &scope,
    std::string_view node,
    std::span<const VulkanPhysicalResourcePlan>
        resources,
    std::span<const VulkanPhysicalAttachmentPlan>
        attachments) {
    if (scope.nodes.empty()) return false;
    for (const auto &attachment : attachments) {
        if (attachment.node != node) continue;
        const auto *physical = findPhysicalResource(
            resources, attachment.logical_resource);
        if (physical == nullptr) return false;
        if (physical->representation !=
                VulkanResourceRepresentation::
                    materialized_image &&
            physical->representation !=
                VulkanResourceRepresentation::external) {
            continue;
        }

        for (auto previous_node =
                 scope.nodes.rbegin();
             previous_node != scope.nodes.rend();
             ++previous_node) {
            const auto same_resource_view =
                std::find_if(
                    attachments.begin(),
                    attachments.end(),
                    [&](const auto &candidate) {
                        return candidate.node ==
                                   *previous_node &&
                               candidate.logical_resource ==
                                   attachment.logical_resource &&
                               candidate.aspect ==
                                   attachment.aspect;
                    });
            if (same_resource_view !=
                    attachments.end() &&
                same_resource_view->subresource !=
                    attachment.subresource) {
                return false;
            }
            const auto previous_attachment =
                std::find_if(
                    attachments.begin(),
                    attachments.end(),
                    [&](const auto &candidate) {
                        return candidate.node ==
                                   *previous_node &&
                               candidate.logical_resource ==
                                   attachment.logical_resource &&
                               candidate.aspect ==
                                   attachment.aspect &&
                               candidate.subresource ==
                                   attachment.subresource;
                    });
            if (previous_attachment ==
                attachments.end()) {
                continue;
            }
            if (previous_attachment->store_op !=
                    VulkanPhysicalAttachmentStoreOp::
                        store ||
                attachment.load_op !=
                    VulkanPhysicalAttachmentLoadOp::
                        load) {
                return false;
            }
            break;
        }
    }
    return true;
}

std::vector<VulkanPhysicalScopePlan> buildPhysicalScopes(
    const CompiledLogicalRenderGraph &canonical_graph,
    const TargetLoweringGraph &workspace,
    std::span<const VulkanPhysicalResourcePlan> resources,
    std::span<const VulkanPhysicalAttachmentPlan>
        attachments,
    const ResolvedVulkanViewExecutionPlan &view_plan,
    bool tile_candidate, const PlanningProfile &profile,
    std::span<const PlanningNodeConstraint> node_constraints,
    std::vector<PlanningDecision> &decisions) {
    std::map<std::string, const LogicalGraphNode *, std::less<>>
        nodes;
    for (const auto &node : workspace.nodes) {
        nodes.emplace(node.logical.name, &node.logical);
    }
    std::map<std::string, std::vector<LogicalDataEdge>, std::less<>>
        incoming_edges;
    for (const auto &edge :
         deriveLogicalDataEdges(canonical_graph)) {
        incoming_edges[edge.consumer_node].push_back(edge);
    }
    std::map<std::string, const LogicalResourceDesc *, std::less<>>
        logical_resources;
    for (const auto &resource : canonical_graph.resources) {
        logical_resources.emplace(resource.name, &resource);
    }
    const auto constraints = nodeConstraintMap(node_constraints);

    std::vector<VulkanPhysicalScopePlan> result;
    for (const auto &lowering_node : workspace.nodes) {
        const auto &node = lowering_node.logical;
        const auto kind = physicalScopeKind(node.kind);
        const auto &node_view = view_plan.requireNode(node.name);
        const auto execution = node_view.execution;
        const auto rasterization_samples =
            kind == VulkanPhysicalScopeKind::rendering
                ? nodeRasterizationSamples(
                      node, logical_resources, resources)
                : 1u;
        bool fuse = false;
        std::vector<std::string> local_reads;
        if (tile_candidate &&
            profile.kind !=
                PlanningProfileKind::conservative_debug &&
            kind == VulkanPhysicalScopeKind::rendering &&
            !result.empty() &&
            result.back().kind ==
                VulkanPhysicalScopeKind::rendering &&
            result.back().view_execution == execution &&
            nodes.at(result.back().nodes.front())
                    ->view_family ==
                node.view_family &&
            result.back().rasterization_samples ==
                rasterization_samples &&
            !constraintBlocksFusion(constraints, node.name)) {
            bool current_scope_blocked = false;
            for (const auto &scope_node : result.back().nodes) {
                if (constraintBlocksFusion(constraints,
                                           scope_node)) {
                    current_scope_blocked = true;
                    break;
                }
            }
            if (!current_scope_blocked) {
                const std::set<std::string, std::less<>>
                    current_nodes(result.back().nodes.begin(),
                                  result.back().nodes.end());
                std::vector<const LogicalDataEdge *> crossing;
                if (const auto found =
                        incoming_edges.find(node.name);
                    found != incoming_edges.end()) {
                    for (const auto &edge : found->second) {
                        if (current_nodes.contains(
                                edge.producer_node)) {
                            crossing.push_back(&edge);
                        }
                    }
                }
                fuse = !crossing.empty();
                for (const auto *edge : crossing) {
                    const auto *resource = findPhysicalResource(
                        resources, edge->value.resource);
                    const auto *use =
                        findNodeUse(node, edge->consumer_port);
                    const auto attachment_read_write =
                        resource != nullptr && use != nullptr &&
                        resource->representation ==
                            VulkanResourceRepresentation::
                                materialized_image &&
                        use->access ==
                            LogicalAccessMode::read_write &&
                        use->footprint.kind ==
                            LogicalReadFootprintKind::
                                same_pixel &&
                        (use->intent ==
                             LogicalAccessIntent::automatic ||
                         use->intent ==
                             LogicalAccessIntent::attachment);
                    const auto tile_local =
                        resource != nullptr &&
                        resource->representation ==
                            VulkanResourceRepresentation::
                                tile_local_attachment;
                    if (!tile_local && !attachment_read_write) {
                        fuse = false;
                        break;
                    }
                    if (tile_local) {
                        local_reads.push_back(
                            edge->value.resource);
                    }
                }
            }
        }
        if (fuse &&
            !canAppendMaterializedRenderingScope(
                result.back(), node.name,
                resources, attachments)) {
            fuse = false;
        }

        if (fuse) {
            auto &scope = result.back();
            scope.nodes.push_back(node.name);
            scope.single_rendering_instance = true;
            scope.local_reads.insert(scope.local_reads.end(),
                                     local_reads.begin(),
                                     local_reads.end());
            scope.region_tags.insert(scope.region_tags.end(),
                                     node.region_tags.begin(),
                                     node.region_tags.end());
            sortAndUnique(scope.local_reads);
            sortAndUnique(scope.region_tags);
            decisions.push_back(PlanningDecision{
                "pelican.plan.rendering_scope_fused@1",
                node.name, scope.id,
                "typed resource access allowed fusion; region tags "
                "were retained only as provenance",
            });
        } else {
            VulkanPhysicalScopePlan scope{
                .id = "scope:" +
                      std::to_string(result.size()),
                .kind = kind,
                .nodes = {node.name},
                .region_tags = node.region_tags,
                .rasterization_samples = rasterization_samples,
                .view_execution = execution,
                .view_count = node_view.view_count,
                .execution_count = node_view.execution_count,
                .view_mask = node_view.view_mask,
            };
            sortAndUnique(scope.region_tags);
            result.push_back(std::move(scope));
        }
    }
    return result;
}

void appendScopeLocalityFailures(
    const CompiledLogicalRenderGraph &canonical_graph,
    CandidateDraft &candidate) {
    std::map<std::string, std::size_t, std::less<>>
        scope_by_node;
    for (std::size_t scope_index = 0;
         scope_index < candidate.scopes.size();
         ++scope_index) {
        for (const auto &node :
             candidate.scopes[scope_index].nodes) {
            scope_by_node.emplace(node, scope_index);
        }
    }

    std::map<std::string,
             const VulkanPhysicalResourcePlan *,
             std::less<>>
        resources;
    for (const auto &resource :
         candidate.resources) {
        resources.emplace(
            resource.logical_resource, &resource);
    }

    std::map<std::string, std::size_t, std::less<>>
        tile_read_uses;
    for (const auto &node : canonical_graph.nodes) {
        for (const auto &use : node.uses) {
            if (!use.input_value) continue;
            const auto resource =
                resources.find(
                    use.input_value->resource);
            if (resource == resources.end() ||
                resource->second->representation !=
                    VulkanResourceRepresentation::
                        tile_local_attachment) {
                continue;
            }
            ++tile_read_uses[
                resource->first];
        }
    }

    std::map<std::string, std::size_t, std::less<>>
        tile_read_edges;
    std::set<std::string, std::less<>>
        invalid_resources;
    for (const auto &edge :
         deriveLogicalDataEdges(canonical_graph)) {
        const auto resource =
            resources.find(edge.value.resource);
        if (resource == resources.end()) continue;
        const auto representation =
            resource->second->representation;
        if (representation !=
                VulkanResourceRepresentation::
                    tile_local_attachment &&
            representation !=
                VulkanResourceRepresentation::
                    transient_attachment) {
            continue;
        }
        const auto producer =
            scope_by_node.find(edge.producer_node);
        const auto consumer =
            scope_by_node.find(edge.consumer_node);
        if (producer == scope_by_node.end() ||
            consumer == scope_by_node.end() ||
            producer->second != consumer->second) {
            invalid_resources.insert(
                edge.value.resource);
            continue;
        }
        if (representation ==
            VulkanResourceRepresentation::
                tile_local_attachment) {
            ++tile_read_edges[edge.value.resource];
            const auto &scope =
                candidate.scopes[consumer->second];
            if (std::find(
                    scope.local_reads.begin(),
                    scope.local_reads.end(),
                    edge.value.resource) ==
                scope.local_reads.end()) {
                invalid_resources.insert(
                    edge.value.resource);
            }
        }
    }
    for (const auto &[resource, read_count] :
         tile_read_uses) {
        if (tile_read_edges[resource] !=
            read_count) {
            invalid_resources.insert(resource);
        }
    }
    for (const auto &resource :
         invalid_resources) {
        candidate.failures.push_back(
            BackendConstraintFailure{
                "pelican.plan.scope_local_resource_escape@1",
                resource,
                "scope-local attachment has a read outside the "
                "fused producer/consumer rendering scope",
            });
    }

    std::map<std::string,
             const LogicalGraphNode *, std::less<>>
        nodes;
    for (const auto &node :
         canonical_graph.nodes) {
        nodes.emplace(node.name, &node);
    }
    for (const auto &scope : candidate.scopes) {
        if (scope.local_reads.empty()) continue;
        std::optional<ResourceExtentPlan>
            attachment_extent;
        bool incompatible = false;
        for (const auto &node_name : scope.nodes) {
            const auto node = nodes.find(node_name);
            if (node == nodes.end()) continue;
            for (const auto &use :
                 node->second->uses) {
                if (!use.output_value ||
                    (use.intent !=
                         LogicalAccessIntent::automatic &&
                     use.intent !=
                         LogicalAccessIntent::attachment)) {
                    continue;
                }
                const auto resource =
                    resources.find(
                        use.output_value->resource);
                if (resource == resources.end() ||
                    !resource->second->extent) {
                    continue;
                }
                if (attachment_extent &&
                    *attachment_extent !=
                        *resource->second->extent) {
                    incompatible = true;
                    break;
                }
                attachment_extent =
                    *resource->second->extent;
            }
            if (incompatible) break;
        }
        if (incompatible) {
            candidate.failures.push_back(
                BackendConstraintFailure{
                    "pelican.plan.scope_attachment_extent_mismatch@1",
                    scope.id,
                    "fused rendering scope attachments have "
                    "different physical extent contracts",
                });
        }
    }
}

void applyResourceViewLayouts(
    const TargetLoweringGraph &workspace,
    const ResolvedVulkanViewExecutionPlan &view_plan,
    CandidateDraft &candidate) {
    std::map<std::string, const TargetLoweringResource *,
             std::less<>>
        lowering_resources;
    for (const auto &resource : workspace.resources) {
        lowering_resources.emplace(resource.logical.name,
                                   &resource);
    }

    for (auto &resource : candidate.resources) {
        const auto lowering =
            lowering_resources.find(resource.logical_resource);
        if (lowering == lowering_resources.end() ||
            lowering->second->logical.type.constructor !=
                LogicalTypeConstructor::image) {
            continue;
        }

        bool multiview_touch = false;
        bool multiview_write = false;
        bool sequential_write = false;
        std::set<std::string, std::less<>>
            writer_families;
        std::set<std::string, std::less<>>
            reader_families;
        for (const auto &node : workspace.nodes) {
            const auto execution =
                view_plan.requireNode(
                    node.logical.name).execution;
            for (const auto &use : node.logical.uses) {
                const auto reads =
                    use.input_value &&
                    use.input_value->resource ==
                        resource.logical_resource;
                const auto writes =
                    use.output_value &&
                    use.output_value->resource ==
                        resource.logical_resource;
                if (!reads && !writes) continue;
                if (reads) {
                    reader_families.insert(
                        node.logical.view_family);
                }
                if (writes) {
                    writer_families.insert(
                        node.logical.view_family);
                }
                if (execution ==
                    VulkanScopeViewExecution::multiview) {
                    multiview_touch = true;
                    multiview_write =
                        multiview_write || writes;
                } else if (
                    execution ==
                        VulkanScopeViewExecution::sequential) {
                    sequential_write =
                        sequential_write || writes;
                }
            }
        }

        const bool crosses_view_families =
            std::any_of(
                writer_families.begin(),
                writer_families.end(),
                [&](const std::string &writer) {
                    return std::any_of(
                        reader_families.begin(),
                        reader_families.end(),
                        [&](const std::string &reader) {
                            return writer != reader;
                        });
                });
        const bool secondary_family_write =
            std::any_of(
                writer_families.begin(),
                writer_families.end(),
                [](const std::string &family) {
                    return family !=
                           mainRenderViewFamilyId;
                });
        if (crosses_view_families) {
            // The producer family's runtime cardinality is independent of
            // the consumer (for example four shadow cascades feeding one or
            // two camera views). Preserve the whole producer-owned array
            // instead of indexing it with the consumer view.
            resource.view_layout =
                VulkanResourceViewLayout::
                    family_2d_array;
        } else if (multiview_write ||
            (multiview_touch && sequential_write)) {
            resource.view_layout =
                VulkanResourceViewLayout::layered_2d_array;
            resource.array_layers =
                std::max(
                    resource.array_layers,
                    view_plan.summary.view_count);
            resource.required_physical_features.push_back(
                std::string{vulkanMultiviewCapability});
            canonicalizeCapabilities(
                resource.required_physical_features,
                "lowered resource physical feature");
            candidate.required_features.push_back(
                std::string{vulkanMultiviewCapability});
        } else if (secondary_family_write) {
            // Secondary-family scopes are one-view physical templates.
            // Their actual provider cardinality is known only at runtime,
            // so resources kept inside that family still need one selectable
            // 2D layer per invocation.
            resource.view_layout =
                VulkanResourceViewLayout::
                    sequential_2d;
        } else if (sequential_write) {
            resource.view_layout =
                VulkanResourceViewLayout::sequential_2d;
        }

        candidate.decisions.push_back(PlanningDecision{
            "pelican.plan.resource_view_layout@1",
            resource.logical_resource,
            std::string{vulkanResourceViewLayoutName(
                resource.view_layout)},
            resource.view_layout ==
                    VulkanResourceViewLayout::layered_2d_array
                ? "multiview output or a sequential-to-multiview "
                  "boundary requires all view layers to coexist"
            : resource.view_layout ==
                      VulkanResourceViewLayout::family_2d_array
                ? "a resource crossing view-family ownership preserves "
                  "the producer family's complete array"
            : secondary_family_write
                ? "a secondary-family resource preserves runtime "
                  "provider views as selectable 2D layers"
            : resource.view_layout ==
                      VulkanResourceViewLayout::sequential_2d
                ? "view-dependent writes execute once per view"
                : "resource is shared by all views",
        });
    }
}

CandidateDraft buildCandidateDraft(
    const CompiledLogicalRenderGraph &canonical_graph,
    const TargetLoweringGraph &workspace,
    const TargetEndpoint &endpoint, bool tile_candidate,
    bool transient_candidate,
    const VulkanTargetPlanRequest &request,
    const ResolvedSampleCountPlan *sample_count_plan,
    const ResolvedVulkanViewExecutionPlan &view_plan,
    const std::optional<ExternalDepthExportSelection>
        &external_depth_export) {
    CandidateDraft result;
    result.name = std::string{
        tile_candidate ? kTileLocalCandidate
        : transient_candidate ? kTransientCandidate
                              : kMaterializedCandidate};
    result.decisions = view_plan.decisions;
    result.required_features = {
        std::string{kGraphicsCapability},
        std::string{kSampledImageCapability},
    };
    result.required_features.insert(
        result.required_features.end(),
        request.required_endpoint_capabilities.begin(),
        request.required_endpoint_capabilities.end());
    if (tile_candidate) {
        result.required_features.insert(
            result.required_features.end(),
            {std::string{kTileBasedCapability},
             std::string{kLocalReadCapability},
             std::string{kTransientAttachmentCapability}});
    }

    result.resources.reserve(workspace.resources.size());
    for (const auto &resource : workspace.resources) {
        if (!resource.lifetime.used) {
            result.decisions.push_back(PlanningDecision{
                "pelican.plan.resource_purged@1",
                resource.logical.name, "unused",
                "resource has no use in the canonical graph",
            });
            continue;
        }

        const auto selected_format =
            selectFormat(resource.pattern, endpoint,
                         resource.logical.type.constructor);
        const auto image =
            resource.logical.type.constructor ==
            LogicalTypeConstructor::image;
        const auto buffer =
            resource.logical.type.constructor ==
            LogicalTypeConstructor::buffer;
        const auto external =
            resource.logical.materialization ==
            LogicalMaterializationRequirement::external;
        const auto exports_depth =
            external_depth_export &&
            external_depth_export->source_resource ==
                resource.logical.name;
        const auto read_requires_materialization =
            resource.uses.widest_read ==
                LogicalReadFootprintKind::neighborhood ||
            resource.uses.widest_read ==
                LogicalReadFootprintKind::arbitrary ||
            resource.uses.widest_read ==
                LogicalReadFootprintKind::temporal;
        const auto tile_local_eligible =
            tile_candidate && image &&
            resource.pattern.allow_tile_local &&
            resolvesToSingleSample(
                resource.logical.name,
                sample_count_plan) &&
            resource.uses.read && resource.uses.written &&
            resource.uses.widest_read ==
                LogicalReadFootprintKind::same_pixel &&
            resource.uses.attachment_access &&
            !resource.uses.storage_access &&
            !resource.uses.transfer_access &&
            !resource.uses.host_access &&
            !resource.uses.non_render_access &&
            !resource.uses.produced_by_snapshot &&
            resource.logical.materialization !=
                LogicalMaterializationRequirement::required &&
            !external && !resource.pattern.require_store &&
            !exports_depth;

        VulkanResourceRepresentation representation;
        if (external) {
            representation =
                VulkanResourceRepresentation::external;
        } else if (buffer) {
            representation =
                VulkanResourceRepresentation::
                    materialized_buffer;
        } else if (!image) {
            // Semantic Value/ObjectSet/Stream resources remain compile-time
            // values in this renderer-only slice and create no Vulkan
            // resource.
            result.decisions.push_back(PlanningDecision{
                "pelican.plan.semantic_resource_elided@1",
                resource.logical.name, "compile_time",
                "non-image/non-buffer logical value has no Vulkan "
                "resource in this vertical slice",
            });
            continue;
        } else if (tile_local_eligible) {
            representation =
                VulkanResourceRepresentation::
                    tile_local_attachment;
        } else if (transient_candidate &&
                   resource.pattern.prefer_transient &&
                   resource.uses.written &&
                   !resource.uses.read &&
                   resource.logical.materialization ==
                       LogicalMaterializationRequirement::
                           virtual_resource &&
                   !resource.pattern.require_store &&
                   !exports_depth &&
                   request.profile.kind !=
                       PlanningProfileKind::
                           conservative_debug &&
                   (!sample_count_plan ||
                    std::none_of(
                        sample_count_plan->resources.begin(),
                        sample_count_plan->resources.end(),
                        [&](const auto &resolved) {
                            return resolved.resource ==
                                       resource.logical.name &&
                                   resolved.samples != 1;
                        }))) {
            representation =
                VulkanResourceRepresentation::
                    transient_attachment;
        } else {
            representation =
                VulkanResourceRepresentation::
                    materialized_image;
        }

        const auto tile_local_was_requested =
            tile_candidate && image &&
            resource.pattern.allow_tile_local &&
            resource.uses.read && resource.uses.written &&
            resource.logical.materialization !=
                LogicalMaterializationRequirement::required &&
            !external;
        if (tile_local_was_requested &&
            !tile_local_eligible &&
            resource.pattern.local_read_fallback ==
                ResourcePatternFallback::reject &&
            !resource.uses.produced_by_snapshot) {
            result.failures.push_back(
                BackendConstraintFailure{
                    "pelican.plan.tile_local_required@1",
                    resource.logical.name,
                    "ResourcePattern rejects materialization fallback "
                    "for read footprint '" +
                        std::string{
                            logicalReadFootprintKindName(
                                resource.uses.widest_read)} +
                        "'",
                });
        } else if (tile_local_was_requested &&
                   !tile_local_eligible) {
            result.diagnostics.push_back(PlanningDiagnostic{
                "pelican.plan.tile_local_fallback@1",
                PlanningDiagnosticSeverity::info,
                resource.logical.name,
                materializationReason(
                    resource, tile_candidate,
                    transient_candidate,
                    tile_local_eligible, representation),
            });
        }

        std::vector<std::string> required_features =
            selected_format.required_capabilities;
        if (representation ==
                VulkanResourceRepresentation::
                    materialized_image ||
            representation ==
                VulkanResourceRepresentation::external) {
            required_features.push_back(
                std::string{kSampledImageCapability});
        } else if (representation ==
                   VulkanResourceRepresentation::
                       materialized_buffer) {
            required_features.push_back(
                std::string{kStorageBufferCapability});
        } else if (representation ==
                   VulkanResourceRepresentation::
                       tile_local_attachment) {
            required_features.insert(
                required_features.end(),
                {std::string{kTileBasedCapability},
                 std::string{kLocalReadCapability},
                 std::string{
                     kTransientAttachmentCapability}});
        } else if (representation ==
                   VulkanResourceRepresentation::
                       transient_attachment) {
            required_features.push_back(
                std::string{
                    kTransientAttachmentCapability});
        }
        if (resource.uses.produced_by_snapshot ||
            resource.uses.transfer_access ||
            exports_depth) {
            required_features.push_back(
                std::string{kTransferCopyCapability});
        }
        canonicalizeCapabilities(
            required_features,
            "lowered resource physical feature");
        result.required_features.insert(
            result.required_features.end(),
            required_features.begin(), required_features.end());

        const auto stored =
            representation ==
                VulkanResourceRepresentation::external ||
            representation ==
                VulkanResourceRepresentation::
                    materialized_image ||
            representation ==
                VulkanResourceRepresentation::
                    materialized_buffer;
        const auto aliasable =
            isMaterialized(representation) &&
            representation !=
                VulkanResourceRepresentation::external &&
            resource.pattern.allow_alias &&
            !resource.pattern.require_store &&
            !exports_depth &&
            resource.logical.materialization !=
                LogicalMaterializationRequirement::external &&
            resource.uses.widest_read !=
                LogicalReadFootprintKind::temporal;
        const auto reason =
            exports_depth
                ? "external depth export requires a materialized, "
                  "non-aliased transfer source"
                : materializationReason(
                      resource, tile_candidate,
                      transient_candidate,
                      tile_local_eligible, representation);
        result.resources.push_back(
            VulkanPhysicalResourcePlan{
                .logical_resource = resource.logical.name,
                .pattern = resource.pattern.id,
                .format = selected_format.format,
                .representation = representation,
                .widest_read = resource.uses.widest_read,
                .lifetime = resource.lifetime,
                .stored = stored,
                .aliasable = aliasable,
                .required_physical_features =
                    std::move(required_features),
                .reason = reason,
                .mip_levels =
                    resource.mip_levels,
                .array_layers =
                    resource.array_layers,
                .dimension =
                    resource.dimension,
                .extent = resource.extent,
            });
        result.decisions.push_back(PlanningDecision{
            "pelican.plan.resource_format_selected@1",
            resource.logical.name, selected_format.format,
            selected_format.format.empty()
                ? "resource constructor has no image format"
                : "highest-priority format candidate supported by "
                  "endpoint facts",
        });
        result.decisions.push_back(PlanningDecision{
            "pelican.plan.resource_representation@1",
            resource.logical.name,
            std::string{vulkanResourceRepresentationName(
                representation)},
            reason,
        });
        if (exports_depth) {
            result.decisions.push_back(PlanningDecision{
                "pelican.plan.external_depth_export_source@1",
                resource.logical.name,
                "materialized_transfer_source",
                external_depth_export->reason,
            });
        }
        if (read_requires_materialization &&
            representation !=
                VulkanResourceRepresentation::
                    materialized_image) {
            result.failures.push_back(
                BackendConstraintFailure{
                    "pelican.plan.read_footprint_not_materialized@1",
                    resource.logical.name,
                    "read footprint requires a materialized image",
                });
        }
    }
    std::sort(
        result.resources.begin(), result.resources.end(),
        [](const VulkanPhysicalResourcePlan &left,
           const VulkanPhysicalResourcePlan &right) {
            return left.logical_resource <
                   right.logical_resource;
        });
    if (sample_count_plan != nullptr) {
        applyResolvedSampleCounts(result, *sample_count_plan);
    }

    for (const auto &node : workspace.nodes) {
        if (node.logical.kind ==
            LogicalGraphNodeKind::snapshot_copy) {
            result.required_features.push_back(
                std::string{kTransferCopyCapability});
        } else if (node.logical.kind ==
                   LogicalGraphNodeKind::compute) {
            result.required_features.push_back(
                std::string{kStorageBufferCapability});
        }
    }
    canonicalizeCapabilities(
        result.required_features,
        "target candidate physical feature");
    result.scopes = buildPhysicalScopes(
        canonical_graph, workspace, result.resources,
        request.automatic_attachments,
        view_plan,
        tile_candidate, request.profile,
        request.node_constraints, result.decisions);
    appendScopeLocalityFailures(
        canonical_graph, result);
    applyResourceViewLayouts(
        workspace, view_plan, result);
    if (external_depth_export &&
        view_plan.summary.uses_multiview) {
        const auto found = std::find_if(
            result.resources.begin(),
            result.resources.end(),
            [&](const VulkanPhysicalResourcePlan &resource) {
                return resource.logical_resource ==
                       external_depth_export
                           ->source_resource;
            });
        if (found == result.resources.end()) {
            throw std::runtime_error(
                "external depth export source was not physically "
                "lowered");
        }
        if (found->array_layers <
            view_plan.summary.view_count) {
            found->view_layout =
                VulkanResourceViewLayout::
                    layered_2d_array;
            found->array_layers =
                view_plan.summary.view_count;
            result.decisions.push_back(
                PlanningDecision{
                    "pelican.plan.external_depth_view_family_storage@1",
                    found->logical_resource,
                    std::to_string(
                        found->array_layers),
                    "view-family submission requires every "
                    "sequential or multiview depth result to "
                    "remain available until the compositor copy",
                });
        }
    }
    if (view_plan.summary.uses_multiview) {
        result.required_features.push_back(
            std::string{vulkanMultiviewCapability});
    }
    canonicalizeCapabilities(
        result.required_features,
        "target candidate physical feature");

    for (const auto &resource : result.resources) {
        if (isMaterialized(resource.representation)) {
            ++result.cost.materialized_resources;
        }
        if (resource.stored) ++result.cost.external_stores;
        if (resource.representation ==
                VulkanResourceRepresentation::
                    tile_local_attachment ||
            resource.representation ==
                VulkanResourceRepresentation::
                    transient_attachment) {
            const auto lowering = std::lower_bound(
                workspace.resources.begin(),
                workspace.resources.end(),
                resource.logical_resource,
                [](const TargetLoweringResource &entry,
                   std::string_view name) {
                    return entry.logical.name < name;
                });
            if (lowering != workspace.resources.end() &&
                lowering->logical.name ==
                    resource.logical_resource) {
                result.cost.transient_bytes +=
                    lowering->pattern.estimated_bytes;
            }
        }
    }
    result.cost.rendering_scopes =
        static_cast<std::uint32_t>(std::count_if(
            result.scopes.begin(), result.scopes.end(),
            [](const VulkanPhysicalScopePlan &scope) {
                return scope.kind ==
                       VulkanPhysicalScopeKind::rendering;
            }));
    const auto uses_tile_local =
        std::any_of(
            result.resources.begin(),
            result.resources.end(),
            [](const auto &resource) {
                return resource.representation ==
                       VulkanResourceRepresentation::
                           tile_local_attachment;
            });
    const auto uses_transient =
        std::any_of(
            result.resources.begin(),
            result.resources.end(),
            [](const auto &resource) {
                return resource.representation ==
                       VulkanResourceRepresentation::
                           transient_attachment;
            });
    result.cost.bandwidth_class =
        uses_tile_local ? 1
        : uses_transient ? 2
                         : 3;
    return result;
}

std::uint32_t maximumColorAttachmentCount(
    const CompiledLogicalRenderGraph &graph,
    std::span<const VulkanPhysicalScopePlan> scopes) {
    std::map<std::string, const LogicalResourceDesc *, std::less<>>
        resources;
    for (const auto &resource : graph.resources) {
        resources.emplace(resource.name, &resource);
    }
    std::map<std::string,
             const LogicalGraphNode *, std::less<>>
        nodes;
    for (const auto &node : graph.nodes) {
        nodes.emplace(node.name, &node);
    }
    std::uint32_t maximum = 0;
    for (const auto &scope : scopes) {
        std::set<std::string, std::less<>> attachments;
        for (const auto &node_name : scope.nodes) {
            const auto node = nodes.find(node_name);
            if (node == nodes.end()) continue;
            for (const auto &use : node->second->uses) {
                if (!use.output_value) continue;
                const auto resource =
                    resources.find(
                        use.output_value->resource);
                if (resource == resources.end() ||
                    resource->second->type.constructor !=
                        LogicalTypeConstructor::image) {
                    continue;
                }
                if (semanticTypeIdName(
                        resource->second->type.semantic) ==
                    "pelican.render.depth@1") {
                    continue;
                }
                if (use.intent ==
                        LogicalAccessIntent::automatic ||
                    use.intent ==
                        LogicalAccessIntent::attachment) {
                    attachments.insert(
                        use.output_value->resource);
                }
            }
        }
        maximum = std::max(
            maximum,
            static_cast<std::uint32_t>(
                attachments.size()));
    }
    return maximum;
}

void applyAttachmentBudget(
    CandidateDraft &candidate,
    const CompiledLogicalRenderGraph &graph,
    const TargetEndpoint &endpoint) {
    const auto budget = endpointUnsignedFact(
        endpoint, kColorAttachmentBudgetFact);
    if (!budget) return;
    const auto required =
        maximumColorAttachmentCount(
            graph, candidate.scopes);
    if (required > *budget) {
        candidate.failures.push_back(
            BackendConstraintFailure{
                "pelican.plan.color_attachment_budget_exceeded@1",
                candidate.name,
                "logical render pass requires " +
                    std::to_string(required) +
                    " color attachments but endpoint '" +
                    endpoint.id + "' reports budget " +
                    std::to_string(*budget),
            });
    }
}

BackendProbeResult probeCandidate(
    const TargetTopologySnapshot &topology,
    const CompilerProviderRegistrySnapshot &providers,
    const VulkanTargetPlanRequest &request,
    const CandidateDraft &draft) {
    BackendProbeInput input{
        .candidate = draft.name,
        .provider = request.provider,
        .endpoint = request.endpoint,
        .required_endpoint_capabilities =
            draft.required_features,
        .required_physical_features =
            draft.required_features,
        .cost = draft.cost,
        .diagnostics = draft.diagnostics,
    };
    auto result = probeVulkanBackend(
        topology, providers, std::move(input));
    result.failures.insert(result.failures.end(),
                           draft.failures.begin(),
                           draft.failures.end());
    std::sort(
        result.failures.begin(), result.failures.end(),
        [](const BackendConstraintFailure &left,
           const BackendConstraintFailure &right) {
            return std::tie(left.id, left.subject, left.detail) <
                   std::tie(right.id, right.subject,
                            right.detail);
        });
    result.feasible = result.failures.empty();
    return result;
}

bool lifetimesDoNotOverlap(const TargetResourceLifetime &left,
                           const TargetResourceLifetime &right) {
    return left.used && right.used &&
           (left.last_use < right.first_use ||
            right.last_use < left.first_use);
}

std::vector<PlanningNamePair> deriveLegalAliasCandidates(
    const TargetLoweringGraph &workspace,
    std::span<const VulkanPhysicalResourcePlan> resources) {
    std::vector<PlanningNamePair> result;
    for (std::size_t left = 0; left < resources.size(); ++left) {
        if (!resources[left].aliasable) continue;
        for (std::size_t right = left + 1;
             right < resources.size(); ++right) {
            if (!resources[right].aliasable ||
                resources[left].representation !=
                    resources[right].representation ||
                resources[left].format !=
                    resources[right].format ||
                resources[left].rasterization_samples !=
                    resources[right].rasterization_samples ||
                resources[left].resolve_required !=
                    resources[right].resolve_required ||
                resources[left].view_layout !=
                    resources[right].view_layout ||
                resources[left].mip_levels !=
                    resources[right].mip_levels ||
                resources[left].array_layers !=
                    resources[right].array_layers ||
                resources[left].dimension !=
                    resources[right].dimension ||
                resources[left].extent !=
                    resources[right].extent ||
                !lifetimesDoNotOverlap(
                    resources[left].lifetime,
                    resources[right].lifetime)) {
                continue;
            }
            const auto left_logical = std::lower_bound(
                workspace.resources.begin(),
                workspace.resources.end(),
                resources[left].logical_resource,
                [](const TargetLoweringResource &entry,
                   std::string_view name) {
                    return entry.logical.name < name;
                });
            const auto right_logical = std::lower_bound(
                workspace.resources.begin(),
                workspace.resources.end(),
                resources[right].logical_resource,
                [](const TargetLoweringResource &entry,
                   std::string_view name) {
                    return entry.logical.name < name;
                });
            if (left_logical == workspace.resources.end() ||
                right_logical == workspace.resources.end() ||
                left_logical->logical.type.constructor !=
                    right_logical->logical.type.constructor) {
                continue;
            }
            result.push_back(PlanningNamePair{
                resources[left].logical_resource,
                resources[right].logical_resource,
            });
        }
    }
    sortAndUnique(result);
    return result;
}

std::vector<VulkanAliasGroupPlan> buildAliasGroups(
    std::span<const PlanningNamePair> selected_pairs) {
    std::set<PlanningNamePair> legal(selected_pairs.begin(),
                                     selected_pairs.end());
    std::vector<std::vector<std::string>> groups;
    const auto pair_is_legal =
        [&](std::string_view left, std::string_view right) {
            PlanningNamePair pair{std::string{left},
                                  std::string{right}};
            if (pair.second < pair.first) {
                std::swap(pair.first, pair.second);
            }
            return legal.contains(pair);
        };

    for (const auto &pair : selected_pairs) {
        auto left_group = groups.end();
        auto right_group = groups.end();
        for (auto iterator = groups.begin();
             iterator != groups.end(); ++iterator) {
            if (std::find(iterator->begin(), iterator->end(),
                          pair.first) != iterator->end()) {
                left_group = iterator;
            }
            if (std::find(iterator->begin(), iterator->end(),
                          pair.second) != iterator->end()) {
                right_group = iterator;
            }
        }
        if (left_group == groups.end() &&
            right_group == groups.end()) {
            groups.push_back({pair.first, pair.second});
        } else if (left_group != groups.end() &&
                   right_group == groups.end()) {
            if (std::all_of(
                    left_group->begin(), left_group->end(),
                    [&](const std::string &member) {
                        return pair_is_legal(member,
                                             pair.second);
                    })) {
                left_group->push_back(pair.second);
            }
        } else if (left_group == groups.end()) {
            if (std::all_of(
                    right_group->begin(), right_group->end(),
                    [&](const std::string &member) {
                        return pair_is_legal(pair.first,
                                             member);
                    })) {
                right_group->push_back(pair.first);
            }
        } else if (left_group != right_group) {
            const auto compatible = std::all_of(
                left_group->begin(), left_group->end(),
                [&](const std::string &left) {
                    return std::all_of(
                        right_group->begin(),
                        right_group->end(),
                        [&](const std::string &right) {
                            return pair_is_legal(left, right);
                        });
                });
            if (compatible) {
                left_group->insert(left_group->end(),
                                   right_group->begin(),
                                   right_group->end());
                groups.erase(right_group);
            }
        }
    }

    for (auto &group : groups) sortAndUnique(group);
    std::sort(groups.begin(), groups.end());
    std::vector<VulkanAliasGroupPlan> result;
    for (std::size_t index = 0; index < groups.size(); ++index) {
        result.push_back(VulkanAliasGroupPlan{
            "alias:" + std::to_string(index),
            std::move(groups[index]),
        });
    }
    return result;
}

const BackendProbeResult &selectedProbe(
    const BackendSelection &selection) {
    const auto found = std::find_if(
        selection.candidates.begin(),
        selection.candidates.end(),
        [&](const BackendProbeResult &candidate) {
            return candidate.candidate ==
                   selection.selected_candidate;
        });
    if (found == selection.candidates.end()) {
        throw std::runtime_error(
            "backend selection does not contain its selected "
            "candidate");
    }
    return *found;
}

void lowerScopeLocalAttachmentStores(
    std::span<const VulkanPhysicalResourcePlan> resources,
    std::span<VulkanPhysicalAttachmentPlan> attachments,
    std::vector<PlanningDecision> &decisions) {
    std::map<std::string, VulkanResourceRepresentation,
             std::less<>>
        virtual_attachments;
    for (const auto &resource : resources) {
        if (resource.representation ==
                VulkanResourceRepresentation::
                    transient_attachment ||
            resource.representation ==
                VulkanResourceRepresentation::
                    tile_local_attachment) {
            virtual_attachments.emplace(
                resource.logical_resource,
                resource.representation);
        }
    }
    for (auto &attachment : attachments) {
        const auto resource =
            virtual_attachments.find(
                attachment.logical_resource);
        if (resource ==
                virtual_attachments.end() ||
            attachment.store_op ==
                VulkanPhysicalAttachmentStoreOp::
                    discard) {
            continue;
        }
        attachment.store_op =
            VulkanPhysicalAttachmentStoreOp::discard;
        const auto tile_local =
            resource->second ==
            VulkanResourceRepresentation::
                tile_local_attachment;
        decisions.push_back(PlanningDecision{
            tile_local
                ? "pelican.plan.tile_local_attachment_store_elided@1"
                : "pelican.plan.transient_attachment_store_elided@1",
            attachment.node + " -> " +
                attachment.logical_resource,
            "discard",
            tile_local
                ? "scope-local attachment contents are consumed "
                  "inside the fused rendering scope, so no external "
                  "store is required"
                : "write-only virtual attachment has no logical "
                  "consumer, so the selected transient backend does "
                  "not store its contents",
        });
    }
}

} // namespace

std::uint64_t vulkanTargetPlanLogicalGraphFingerprint(
    const CompiledLogicalRenderGraph &graph) {
    StableFingerprint64 fingerprint;
    fingerprint.appendString(
        "pelican.vulkan_target_plan.logical_graph@2");
    appendLogicalGraphFingerprintPayload(
        fingerprint, graph);
    return fingerprint.value();
}

VulkanTargetPlanPinPackage ejectVulkanTargetPlanPinPackage(
    const VulkanTargetPlan &plan) {
    requireNonEmpty(plan.graph, "Vulkan target plan graph");
    requireNonEmpty(
        plan.backend_selection.selected_candidate,
        "Vulkan target plan selected backend candidate");
    return {
        .graph = plan.graph,
        .logical_graph_fingerprint =
            plan.logical_graph_fingerprint,
        .backend_candidate =
            plan.backend_selection.selected_candidate,
    };
}

nlohmann::ordered_json vulkanTargetPlanPinPackageToJson(
    const VulkanTargetPlanPinPackage &package) {
    requireNonEmpty(
        package.graph, "Vulkan target plan pin graph");
    requireVersionedName(
        package.backend_candidate,
        "Vulkan target plan pinned backend candidate");
    return {
        {"schema", kPinPackageSchema},
        {"version", kPinPackageVersion},
        {"graph", package.graph},
        {"logical_graph_fingerprint",
         stableFingerprint64String(
             package.logical_graph_fingerprint)},
        {"pins",
         nlohmann::ordered_json{
             {"backend_candidate",
              package.backend_candidate},
         }},
    };
}

VulkanTargetPlanPinPackage
vulkanTargetPlanPinPackageFromJson(
    const nlohmann::json &document) {
    constexpr std::string_view context =
        "Vulkan target plan pin package";
    if (!document.is_object()) {
        throw std::runtime_error(
            std::string{context} + " must be an object");
    }
    requireOnlyKeys(
        document,
        {"schema", "version", "graph",
         "logical_graph_fingerprint", "pins"},
        context);
    if (requireJsonString(
            document, "schema", context) !=
        kPinPackageSchema) {
        throw std::runtime_error(
            std::string{context} + " schema must be '" +
            std::string{kPinPackageSchema} + "'");
    }
    const auto version = document.find("version");
    if (version == document.end() ||
        !version->is_number_integer() ||
        version->get<std::int64_t>() !=
            kPinPackageVersion) {
        throw std::runtime_error(
            std::string{context} +
            " version must be exactly 1");
    }
    const auto fingerprint =
        document.find("logical_graph_fingerprint");
    if (fingerprint == document.end()) {
        throw std::runtime_error(
            std::string{context} +
            " requires logical_graph_fingerprint");
    }
    const auto pins = document.find("pins");
    if (pins == document.end() || !pins->is_object()) {
        throw std::runtime_error(
            std::string{context} +
            " pins must be an object");
    }
    requireOnlyKeys(
        *pins, {"backend_candidate"},
        "Vulkan target plan pin package pins");

    VulkanTargetPlanPinPackage result{
        .graph =
            requireJsonString(document, "graph", context),
        .logical_graph_fingerprint =
            parseFingerprint(
                *fingerprint,
                "Vulkan target plan pin package "
                "logical_graph_fingerprint"),
        .backend_candidate =
            requireJsonString(
                *pins, "backend_candidate",
                "Vulkan target plan pin package pins"),
    };
    requireVersionedName(
        result.backend_candidate,
        "Vulkan target plan pinned backend candidate");
    return result;
}

void validateVulkanPhysicalFeatureClosure(
    const BackendProbeResult &selected_probe,
    std::span<const std::string> lowered_required_features) {
    if (!selected_probe.feasible) {
        throw std::runtime_error(
            "physical feature closure requires a feasible selected "
            "probe");
    }
    auto declared =
        selected_probe.required_physical_features;
    auto lowered = std::vector<std::string>(
        lowered_required_features.begin(),
        lowered_required_features.end());
    canonicalizeCapabilities(
        declared, "selected probe physical feature");
    canonicalizeCapabilities(
        lowered, "lowered physical feature");
    std::vector<std::string> growth;
    std::set_difference(
        lowered.begin(), lowered.end(), declared.begin(),
        declared.end(), std::back_inserter(growth));
    if (!growth.empty()) {
        std::ostringstream detail;
        for (std::size_t index = 0; index < growth.size();
             ++index) {
            if (index != 0) detail << ", ";
            detail << growth[index];
        }
        throw std::runtime_error(
            "pelican.plan.lowering_capability_growth@1: selected "
            "probe for '" +
            selected_probe.candidate +
            "' did not declare lowered physical feature(s): " +
            detail.str());
    }
}

VulkanTargetPlan compileVulkanTargetPlan(
    const LogicalTypeRegistry &types,
    const CompiledLogicalRenderGraph &canonical_graph,
    const TargetTopologySnapshot &source_topology,
    const CompilerProviderRegistrySnapshot &providers,
    VulkanTargetPlanRequest request) {
    requireNonEmpty(request.endpoint,
                    "Vulkan target plan endpoint");
    requireVersionedName(request.provider,
                         "Vulkan target plan provider");
    canonicalizeCapabilities(
        request.required_endpoint_capabilities,
        "Vulkan target plan required endpoint capability");
    const auto logical_graph_fingerprint =
        vulkanTargetPlanLogicalGraphFingerprint(
            canonical_graph);
    std::optional<std::string_view>
        requested_backend_candidate;
    if (request.pin_package) {
        if (request.pin_package->graph !=
            canonical_graph.name) {
            throw std::runtime_error(
                "Vulkan target plan pin package graph mismatch: "
                "expected '" +
                canonical_graph.name + "', got '" +
                request.pin_package->graph + "'");
        }
        if (request.pin_package
                ->logical_graph_fingerprint !=
            logical_graph_fingerprint) {
            throw std::runtime_error(
                "Vulkan target plan pin package is stale for graph '" +
                canonical_graph.name + "': expected " +
                stableFingerprint64String(
                    logical_graph_fingerprint) +
                ", got " +
                stableFingerprint64String(
                    request.pin_package
                        ->logical_graph_fingerprint));
        }
        requireVersionedName(
            request.pin_package->backend_candidate,
            "Vulkan target plan pinned backend candidate");
        requested_backend_candidate =
            request.pin_package->backend_candidate;
    }
    if (request.fragment_package) {
        if (request.fragment_package->graph !=
            canonical_graph.name) {
            throw std::runtime_error(
                "Vulkan physical fragment graph mismatch: expected '" +
                canonical_graph.name + "', got '" +
                request.fragment_package->graph + "'");
        }
        if (request.fragment_package
                ->logical_graph_fingerprint !=
            logical_graph_fingerprint) {
            throw std::runtime_error(
                "Vulkan physical fragment is stale for logical graph '" +
                canonical_graph.name + "'");
        }
        requireVersionedName(
            request.fragment_package->backend_candidate,
            "Vulkan physical fragment backend candidate");
        if (requested_backend_candidate &&
            *requested_backend_candidate !=
                request.fragment_package
                    ->backend_candidate) {
            throw std::runtime_error(
                "Vulkan target plan pin and physical fragment "
                "select different backend candidates");
        }
        requested_backend_candidate =
            request.fragment_package
                ->backend_candidate;
    }
    const auto topology =
        canonicalizeTargetTopology(source_topology);
    const auto &endpoint =
        requireEndpoint(topology, request.endpoint);
    requireVulkanEndpointCapabilities(
        endpoint, request.required_endpoint_capabilities);
    const auto view_execution = resolveVulkanViewExecutionPlan(
        canonical_graph, endpoint, request.view_execution);

    LogicalPlanningOpportunityInput initial_input{
        .profile = request.profile,
        .node_constraints = request.node_constraints,
        .resource_constraints =
            request.resource_constraints,
    };
    const auto initial_opportunities =
        analyzeLogicalPlanningOpportunities(
            canonical_graph, initial_input);
    auto workspace = makeTargetLoweringGraph(
        types, canonical_graph, request.pattern_bindings,
        initial_opportunities.node_order);
    const auto external_depth_export =
        selectExternalDepthExport(
            types, workspace,
            request.external_depth_export);
    auto sample_count_plan =
        request.sample_count
            ? std::optional<ResolvedSampleCountPlan>{
                  resolveTargetSampleCounts(
                      canonical_graph, *request.sample_count)}
            : std::nullopt;

    auto materialized = buildCandidateDraft(
        canonical_graph, workspace, endpoint, false, false,
        request,
        sample_count_plan ? &*sample_count_plan : nullptr,
        view_execution, external_depth_export);
    auto transient = buildCandidateDraft(
        canonical_graph, workspace, endpoint, false, true,
        request,
        sample_count_plan ? &*sample_count_plan : nullptr,
        view_execution, external_depth_export);
    auto tile_local = buildCandidateDraft(
        canonical_graph, workspace, endpoint, true, true,
        request,
        sample_count_plan ? &*sample_count_plan : nullptr,
        view_execution, external_depth_export);
    applyAttachmentBudget(materialized, canonical_graph,
                          endpoint);
    applyAttachmentBudget(transient, canonical_graph,
                          endpoint);
    applyAttachmentBudget(tile_local, canonical_graph,
                          endpoint);

    auto materialized_probe = probeCandidate(
        topology, providers, request, materialized);
    auto transient_probe = probeCandidate(
        topology, providers, request, transient);
    auto tile_probe = probeCandidate(
        topology, providers, request, tile_local);
    auto selection = selectBackendCandidate(
        {std::move(materialized_probe),
         std::move(transient_probe),
         std::move(tile_probe)},
        request.diagnostic_policy,
        requested_backend_candidate);
    const auto selected_name = selection.selected_candidate;
    const auto &selected_draft =
        selected_name == tile_local.name ? tile_local
        : selected_name == transient.name ? transient
                                          : materialized;
    if (selected_name != tile_local.name &&
        selected_name != transient.name &&
        selected_name != materialized.name) {
        throw std::runtime_error(
            "target planner selected an unknown finite "
            "candidate: " +
            selected_name);
    }

    const auto legal_aliases = deriveLegalAliasCandidates(
        workspace, selected_draft.resources);
    LogicalPlanningOpportunityInput final_input{
        .profile = request.profile,
        .node_constraints =
            std::move(request.node_constraints),
        .resource_constraints =
            std::move(request.resource_constraints),
        .legal_alias_candidates = legal_aliases,
    };
    auto opportunities =
        analyzeLogicalPlanningOpportunities(
            canonical_graph, std::move(final_input));
    auto alias_groups =
        buildAliasGroups(opportunities.alias_candidates);

    std::map<std::string,
             const VulkanPhysicalResourcePlan *,
             std::less<>>
        physical_resources;
    for (const auto &resource : selected_draft.resources) {
        physical_resources.emplace(resource.logical_resource,
                                   &resource);
    }
    for (auto &resource : workspace.resources) {
        if (const auto physical =
                physical_resources.find(
                    resource.logical.name);
            physical != physical_resources.end()) {
            resource.required_physical_features =
                physical->second
                    ->required_physical_features;
        }
    }
    for (auto &node : workspace.nodes) {
        if (node.logical.kind ==
            LogicalGraphNodeKind::snapshot_copy) {
            node.required_physical_features = {
                std::string{kTransferCopyCapability}};
        } else if (node.logical.kind ==
                   LogicalGraphNodeKind::compute) {
            node.required_physical_features = {
                std::string{kStorageBufferCapability}};
        } else {
            node.required_physical_features = {
                std::string{kGraphicsCapability}};
        }
        if (view_execution.requireNode(
                node.logical.name).execution ==
            VulkanScopeViewExecution::multiview) {
            node.required_physical_features.push_back(
                std::string{vulkanMultiviewCapability});
            canonicalizeCapabilities(
                node.required_physical_features,
                "lowered node physical feature");
        }
    }

    lowerTargetExecutionDialect(workspace);
    validateVulkanPhysicalFeatureClosure(
        selectedProbe(selection),
        selected_draft.required_features);
    lowerVulkanPhysicalDialect(workspace);

    auto decisions = selected_draft.decisions;
    for (const auto &group : alias_groups) {
        decisions.push_back(PlanningDecision{
            "pelican.plan.alias_group_selected@1",
            group.id, std::to_string(group.resources.size()),
            "all members have compatible representations and "
            "pairwise non-overlapping logical lifetimes",
        });
    }
    decisions.push_back(PlanningDecision{
        "pelican.plan.target_candidate_materialized@1",
        canonical_graph.name, selection.selected_candidate,
        "selected probe features were closed before "
        "physical.vulkan lowering",
    });

    std::optional<VulkanExternalDepthExportPlan>
        external_depth_plan;
    if (external_depth_export) {
        const auto *resource = findPhysicalResource(
            selected_draft.resources,
            external_depth_export->source_resource);
        if (resource == nullptr ||
            resource->representation !=
                VulkanResourceRepresentation::
                    materialized_image ||
            !resource->stored ||
            resource->format.empty()) {
            throw std::runtime_error(
                "external depth export did not lower to a stored "
                "materialized image: " +
                external_depth_export->source_resource);
        }
        external_depth_plan =
            VulkanExternalDepthExportPlan{
                .source_resource =
                    resource->logical_resource,
                .format = resource->format,
                .view_layout =
                    resource->view_layout,
                .array_layers =
                    resource->array_layers,
                .reason =
                    external_depth_export->reason,
            };
    }

    VulkanTargetPlan result{
        .graph = canonical_graph.name,
        .logical_graph_fingerprint =
            logical_graph_fingerprint,
        .graph_transforms =
            canonical_graph.graph_transforms,
        .subgraph_replacements =
            canonical_graph.subgraph_replacements,
        .render_strategy =
            canonical_graph.render_strategy,
        .backend_selection = std::move(selection),
        .opportunities = std::move(opportunities),
        .lowering_graph = std::move(workspace),
        .resources = selected_draft.resources,
        .scopes = selected_draft.scopes,
        .attachments =
            std::move(request.automatic_attachments),
        .alias_groups = std::move(alias_groups),
        .required_physical_features =
            selected_draft.required_features,
        .decisions = std::move(decisions),
        .sample_count_plan = std::move(sample_count_plan),
        .view_execution_plan = view_execution.summary,
        .external_depth_export =
            std::move(external_depth_plan),
        .applied_pin_package =
            std::move(request.pin_package),
    };
    lowerScopeLocalAttachmentStores(
        result.resources, result.attachments,
        result.decisions);
    validateVulkanPhysicalAttachmentPlans(
        canonical_graph, result.resources,
        result.attachments);
    result.automatic_plan_fingerprint =
        vulkanAutomaticTargetPlanFingerprint(
            topology, result);
    if (request.fragment_package) {
        result = linkVulkanPhysicalFragment(
            canonical_graph, topology,
            std::move(result),
            std::move(*request.fragment_package),
            request.fragment_format_capabilities);
    }
    return result;
}

nlohmann::ordered_json vulkanTargetPlanToJson(
    const VulkanTargetPlan &plan) {
    validateTargetLoweringGraphDialect(
        plan.lowering_graph,
        TargetLoweringStage::vulkan_physical_complete);
    validateVulkanPhysicalFeatureClosure(
        selectedProbe(plan.backend_selection),
        plan.required_physical_features);

    nlohmann::ordered_json result{
        {"schema", "pelican.vulkan_target_plan"},
        {"version", 1},
        {"graph", plan.graph},
        {"logical_graph_fingerprint",
         stableFingerprint64String(
             plan.logical_graph_fingerprint)},
        {"automatic_plan_fingerprint",
         stableFingerprint64String(
             plan.automatic_plan_fingerprint)},
        {"ejectable_pin_package",
         vulkanTargetPlanPinPackageToJson(
             ejectVulkanTargetPlanPinPackage(plan))},
        {"ejectable_physical_fragment",
         vulkanPhysicalFragmentPackageToJson(
             ejectVulkanPhysicalFragmentPackage(
                 plan))},
        {"ejectable_complete_physical_plan",
         vulkanCompletePhysicalPlanPackageToJson(
             ejectVulkanCompletePhysicalPlanPackage(
                 plan))},
        {"graph_transforms",
         nlohmann::ordered_json::array()},
        {"subgraph_replacements",
         nlohmann::ordered_json::array()},
        {"backend_selection",
         backendSelectionToJson(plan.backend_selection)},
        {"planning_opportunities",
         logicalPlanningOpportunityReportToJson(
             plan.opportunities)},
        {"lowering_graph",
         targetLoweringGraphToJson(plan.lowering_graph)},
        {"required_physical_features",
         plan.required_physical_features},
        {"view_execution_plan",
         nlohmann::ordered_json{
             {"view_count",
              plan.view_execution_plan.view_count},
             {"requested",
              xrViewExecutionPreferenceName(
                  plan.view_execution_plan.requested)},
             {"endpoint_supports_multiview",
              plan.view_execution_plan
                  .endpoint_supports_multiview},
             {"max_multiview_view_count",
              plan.view_execution_plan
                  .max_multiview_view_count},
             {"uses_multiview",
              plan.view_execution_plan.uses_multiview},
             {"mixed_execution",
              plan.view_execution_plan.mixed_execution},
             {"auto_gate",
              resolvedXrMultiviewAutoPolicyToJson(
                  plan.view_execution_plan
                      .automatic_policy)},
             {"reason", plan.view_execution_plan.reason},
         }},
    };
    if (plan.applied_pin_package) {
        result["applied_pin_package"] =
            vulkanTargetPlanPinPackageToJson(
                *plan.applied_pin_package);
    }
    if (plan.applied_fragment_package) {
        result["applied_physical_fragment"] =
            vulkanPhysicalFragmentPackageToJson(
                *plan.applied_fragment_package);
    }
    for (const auto &selection :
         plan.graph_transforms) {
        result["graph_transforms"].push_back(
            nlohmann::ordered_json{
                {"name", selection.name},
                {"provider", selection.provider},
                {"implementation",
                 selection.implementation},
                {"contract", selection.contract},
                {"boundary_fingerprint",
                 selection.boundary_fingerprint},
                {"input_graph_fingerprint",
                 selection.input_graph_fingerprint},
                {"output_graph_fingerprint",
                 selection.output_graph_fingerprint},
                {"provider_owner",
                 selection.provider_owner},
                {"provider_identity",
                 selection.provider_identity},
                {"provider_generation",
                 selection.provider_generation},
                {"provider_version",
                 selection.provider_version},
                {"provider_capability_bits",
                 selection.provider_capability_bits},
                {"transform_index",
                 selection.transform_index},
                {"explicitly_selected",
                 selection.explicitly_selected},
            });
    }
    if (plan.render_strategy) {
        const auto &selection =
            *plan.render_strategy;
        result["render_strategy"] =
            nlohmann::ordered_json{
                {"name", selection.name},
                {"provider", selection.provider},
                {"implementation",
                 selection.implementation},
                {"contract", selection.contract},
                {"output_contract",
                 selection.output_contract},
                {"graph_variant",
                 selection.graph_variant},
                {"facade_capability_bits",
                 selection.facade_capability_bits},
                {"input_config_fingerprint",
                 selection.input_config_fingerprint},
                {"output_config_fingerprint",
                 selection.output_config_fingerprint},
                {"provider_owner",
                 selection.provider_owner},
                {"provider_identity",
                 selection.provider_identity},
                {"provider_generation",
                 selection.provider_generation},
                {"provider_version",
                 selection.provider_version},
                {"provider_capability_bits",
                 selection.provider_capability_bits},
                {"explicitly_selected",
                 selection.explicitly_selected},
            };
    }
    if (plan.resolution_plan) {
        const auto encode_extent =
            [](const ResourceExtentPlan &extent) {
                return nlohmann::ordered_json{
                    {"kind",
                     resourceExtentKindName(extent.kind)},
                    {"scale_x", extent.scale_x},
                    {"scale_y", extent.scale_y},
                    {"width", extent.width},
                    {"height", extent.height},
                };
            };
        result["resolution_plan"] =
            nlohmann::ordered_json{
                {"render_source_resource",
                 plan.resolution_plan
                     ->render_source_resource},
                {"render_extent",
                 encode_extent(
                     plan.resolution_plan
                         ->render_extent)},
                {"output_source_resource",
                 plan.resolution_plan
                     ->output_source_resource},
                {"output_extent",
                 encode_extent(
                     plan.resolution_plan
                         ->output_extent)},
                {"scene_resources",
                 plan.resolution_plan
                     ->scene_resources},
            };
    }
    if (plan.external_depth_export) {
        result["external_depth_export"] =
            nlohmann::ordered_json{
                {"source_resource",
                 plan.external_depth_export
                     ->source_resource},
                {"format",
                 plan.external_depth_export->format},
                {"view_layout",
                 vulkanResourceViewLayoutName(
                     plan.external_depth_export
                         ->view_layout)},
                {"array_layers",
                 plan.external_depth_export
                     ->array_layers},
                {"reason",
                 plan.external_depth_export->reason},
            };
    }
    for (const auto &selection :
         plan.subgraph_replacements) {
        result["subgraph_replacements"].push_back(
            nlohmann::ordered_json{
                {"region", selection.region},
                {"provider", selection.provider},
                {"implementation",
                 selection.implementation},
                {"contract", selection.contract},
                {"contract_fingerprint",
                 selection.contract_fingerprint},
                {"provider_owner",
                 selection.provider_owner},
                {"provider_identity",
                 selection.provider_identity},
                {"provider_generation",
                 selection.provider_generation},
                {"provider_version",
                 selection.provider_version},
                {"provider_capability_bits",
                 selection.provider_capability_bits},
                {"explicitly_selected",
                 selection.explicitly_selected},
                {"source_nodes",
                 selection.source_nodes},
                {"replacement_nodes",
                 selection.replacement_nodes},
            });
    }
    result["resources"] = nlohmann::ordered_json::array();
    for (const auto &resource : plan.resources) {
        nlohmann::ordered_json lifetime{
            {"used", resource.lifetime.used}};
        if (resource.lifetime.used) {
            lifetime["first_use"] =
                resource.lifetime.first_use;
            lifetime["last_use"] =
                resource.lifetime.last_use;
        }
        nlohmann::ordered_json resource_json{
                {"logical_resource",
                 resource.logical_resource},
                {"pattern", resource.pattern},
                {"format", resource.format},
                {"representation",
                 vulkanResourceRepresentationName(
                     resource.representation)},
                {"widest_read",
                 logicalReadFootprintKindName(
                     resource.widest_read)},
                {"lifetime", std::move(lifetime)},
                {"stored", resource.stored},
                {"aliasable", resource.aliasable},
                {"required_physical_features",
                 resource.required_physical_features},
                {"reason", resource.reason},
                {"view_layout",
                 vulkanResourceViewLayoutName(
                     resource.view_layout)},
                {"array_layers",
                 resource.array_layers},
                {"dimension",
                 imageResourceDimensionName(
                     resource.dimension)},
                {"mip_levels",
                 nlohmann::ordered_json{
                     {"mode",
                      imageMipLevelModeName(
                          resource.mip_levels.mode)},
                     {"count",
                      resource.mip_levels.count},
                 }},
                {"extent",
                 resource.extent
                     ? nlohmann::ordered_json{
                           {"kind",
                            resourceExtentKindName(
                                resource.extent->kind)},
                           {"scale_x",
                            resource.extent->scale_x},
                           {"scale_y",
                            resource.extent->scale_y},
                           {"width",
                            resource.extent->width},
                           {"height",
                            resource.extent->height},
                       }
                     : nlohmann::ordered_json(nullptr)},
        };
        if (plan.sample_count_plan) {
            resource_json["rasterization_samples"] =
                resource.rasterization_samples;
            resource_json["resolve_required"] =
                resource.resolve_required;
        }
        result["resources"].push_back(std::move(resource_json));
    }
    result["scopes"] = nlohmann::ordered_json::array();
    for (const auto &scope : plan.scopes) {
        nlohmann::ordered_json scope_json{
                {"id", scope.id},
                {"kind",
                 vulkanPhysicalScopeKindName(scope.kind)},
                {"nodes", scope.nodes},
                {"single_rendering_instance",
                 scope.single_rendering_instance},
                {"local_reads", scope.local_reads},
                {"regions", scope.region_tags},
                {"view_execution",
                 vulkanScopeViewExecutionName(
                     scope.view_execution)},
                {"view_count", scope.view_count},
                {"execution_count",
                 scope.execution_count},
                {"view_mask", scope.view_mask},
            };
        if (plan.sample_count_plan) {
            scope_json["rasterization_samples"] =
                scope.rasterization_samples;
        }
        result["scopes"].push_back(std::move(scope_json));
    }
    if (!plan.attachments.empty()) {
        result["attachments"] =
            nlohmann::ordered_json::array();
        for (const auto &attachment :
             plan.attachments) {
            nlohmann::ordered_json entry{
                {"node", attachment.node},
                {"logical_resource",
                 attachment.logical_resource},
                {"aspect",
                 vulkanPhysicalAttachmentAspectName(
                     attachment.aspect)},
                {"load_op",
                 vulkanPhysicalAttachmentLoadOpName(
                     attachment.load_op)},
                {"store_op",
                 vulkanPhysicalAttachmentStoreOpName(
                     attachment.store_op)},
            };
            if (attachment.subresource) {
                entry["subresource"] =
                    imageSubresourceToJson(
                        *attachment.subresource);
            }
            result["attachments"].push_back(
                std::move(entry));
        }
    }
    result["alias_groups"] =
        nlohmann::ordered_json::array();
    for (const auto &group : plan.alias_groups) {
        result["alias_groups"].push_back(
            nlohmann::ordered_json{
                {"id", group.id},
                {"resources", group.resources},
            });
    }
    result["decisions"] = nlohmann::ordered_json::array();
    for (const auto &decision : plan.decisions) {
        result["decisions"].push_back(
            nlohmann::ordered_json{
                {"id", decision.id},
                {"subject", decision.subject},
                {"selected", decision.selected},
                {"detail", decision.detail},
            });
    }
    if (plan.sample_count_plan) {
        result["sample_count_plan"] =
            resolvedSampleCountPlanToJson(
                *plan.sample_count_plan);
    }
    return result;
}

} // namespace Pelican
