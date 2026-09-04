#include "prefabprovider.hpp"

#include "componentcodec.hpp"

#include "../userpublic/details/behavior/registerer.hpp"
#include "../ecs/componentinfo.hpp"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>

namespace Pelican {
namespace {

using OJson = nlohmann::ordered_json;

OJson f32(float value) { return OJson(value); }

void appendU32(std::vector<unsigned char> &bytes, std::uint32_t value) {
    for (int shift = 0; shift != 32; shift += 8) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}

void appendU64(std::vector<unsigned char> &bytes, std::uint64_t value) {
    for (int shift = 0; shift != 64; shift += 8) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}

void appendString(std::vector<unsigned char> &bytes, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("provider descriptor string is too large");
    }
    appendU32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

template <class Unsigned>
void appendLittle(std::vector<unsigned char> &bytes, Unsigned value,
                  std::size_t width = sizeof(Unsigned)) {
    for (std::size_t index = 0; index < width; ++index) {
        bytes.push_back(static_cast<unsigned char>(value & 0xffU));
        value >>= 8U;
    }
}

void appendTypedValue(std::vector<unsigned char> &bytes,
                      Schema::FieldType type, const OJson &value) {
    using T = Schema::FieldType;
    bytes.push_back(static_cast<unsigned char>(type));
    const auto appendSigned = [&](std::size_t width) {
        appendLittle(bytes,
                     static_cast<std::uint64_t>(value.get<std::int64_t>()),
                     width);
    };
    const auto appendUnsigned = [&](std::size_t width) {
        appendLittle(bytes, value.get<std::uint64_t>(), width);
    };
    const auto appendF32 = [&](const OJson &number) {
        auto scalar = static_cast<float>(number.get<double>());
        if (scalar == 0.0F) scalar = 0.0F;
        appendLittle(bytes, std::bit_cast<std::uint32_t>(scalar));
    };
    switch (type) {
    case T::I8: appendSigned(1); break;
    case T::I16: appendSigned(2); break;
    case T::I32: appendSigned(4); break;
    case T::I64: appendSigned(8); break;
    case T::U8: appendUnsigned(1); break;
    case T::U16: appendUnsigned(2); break;
    case T::U32: appendUnsigned(4); break;
    case T::U64: appendUnsigned(8); break;
    case T::F32: appendF32(value); break;
    case T::F64: {
        auto scalar = value.get<double>();
        if (scalar == 0.0) scalar = 0.0;
        appendLittle(bytes, std::bit_cast<std::uint64_t>(scalar));
        break;
    }
    case T::Vec2:
    case T::Vec3:
    case T::Vec4:
    case T::Quat:
        for (const auto &element : value) appendF32(element);
        break;
    case T::String:
    case T::Enum:
        appendString(bytes, value.get_ref<const std::string &>());
        break;
    case T::Bool: bytes.push_back(value.get<bool>() ? 1U : 0U); break;
    }
}

// Applicability values carry their own wire tag because their field type is
// intentionally not looked up through another descriptor.
void appendDiscriminantValue(std::vector<unsigned char> &bytes,
                             const OJson &value) {
    if (value.is_boolean()) {
        bytes.push_back(0x01U);
        bytes.push_back(value.get<bool>() ? 1U : 0U);
    } else if (value.is_number_unsigned()) {
        bytes.push_back(0x02U);
        appendLittle(bytes, value.get<std::uint64_t>());
    } else if (value.is_number_integer()) {
        bytes.push_back(0x03U);
        appendLittle(bytes,
                     static_cast<std::uint64_t>(value.get<std::int64_t>()));
    } else if (value.is_number_float()) {
        bytes.push_back(0x04U);
        auto scalar = value.get<double>();
        if (scalar == 0.0) scalar = 0.0;
        appendLittle(bytes, std::bit_cast<std::uint64_t>(scalar));
    } else if (value.is_string()) {
        bytes.push_back(0x05U);
        appendString(bytes, value.get_ref<const std::string &>());
    } else {
        throw std::logic_error(
            "provider discriminant must be a scalar canonical value");
    }
}

void appendRange(std::vector<unsigned char> &bytes,
                 const Schema::FieldRange &range) {
    std::visit([&](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<T, std::monostate>) {
            bytes.push_back(0);
        } else if constexpr (std::same_as<T, Schema::SignedRange>) {
            bytes.push_back(1);
            appendU64(bytes, std::bit_cast<std::uint64_t>(value.min));
            appendU64(bytes, std::bit_cast<std::uint64_t>(value.max));
        } else if constexpr (std::same_as<T, Schema::UnsignedRange>) {
            bytes.push_back(2);
            appendU64(bytes, value.min);
            appendU64(bytes, value.max);
        } else {
            bytes.push_back(3);
            const auto encode = [&](double number) {
                if (number == 0.0) number = 0.0;
                appendU64(bytes, std::bit_cast<std::uint64_t>(number));
            };
            encode(value.min);
            encode(value.max);
        }
    }, range);
}

void appendSchemaNode(std::vector<unsigned char> &bytes,
                      const Schema::Node &node) {
    bytes.push_back(static_cast<unsigned char>(node.tag));
    appendString(bytes, node.name);
    bytes.push_back(static_cast<unsigned char>(node.presence));
    bytes.push_back(node.leaf ? 1U : 0U);
    if (node.leaf) {
        bytes.push_back(static_cast<unsigned char>(node.leaf->type));
        appendRange(bytes, node.leaf->range);
        appendString(bytes, node.leaf->unit);
    }
    bytes.push_back(node.default_value ? 1U : 0U);
    if (node.default_value) appendString(bytes, node.default_value->dump());
    appendU32(bytes, static_cast<std::uint32_t>(node.vocabulary.size()));
    for (const auto &value : node.vocabulary) {
        appendString(bytes, value.name);
        appendU64(bytes, value.value);
        bytes.push_back(value.signed_value ? 1U : 0U);
    }
    appendU32(bytes, static_cast<std::uint32_t>(node.children.size()));
    for (const auto &child : node.children) appendSchemaNode(bytes, child);
}

void appendDescriptorRecord(std::vector<unsigned char> &bytes,
                            const BindableDescriptor &descriptor) {
    appendString(bytes, descriptor.provider_name);
    appendString(bytes, descriptor.json_pointer);
    bytes.push_back(static_cast<unsigned char>(descriptor.type));
    bytes.push_back(static_cast<unsigned char>(descriptor.semantic_kind));
    appendString(bytes, descriptor.asset_kind);
    bytes.push_back(descriptor.bindable ? 1U : 0U);
    bytes.push_back(descriptor.required ? 1U : 0U);
    bytes.push_back(descriptor.canonical_default ? 1U : 0U);
    if (descriptor.canonical_default) {
        appendTypedValue(bytes, descriptor.type,
                         *descriptor.canonical_default);
    }
    appendRange(bytes, descriptor.range);
    auto enums = descriptor.enum_values;
    std::sort(enums.begin(), enums.end());
    appendU32(bytes, static_cast<std::uint32_t>(enums.size()));
    for (const auto &value : enums) appendString(bytes, value);

    auto applicability = descriptor.applicability;
    std::sort(applicability.begin(), applicability.end(),
              [](const auto &lhs, const auto &rhs) {
                  return lhs.json_pointer < rhs.json_pointer;
              });
    appendU32(bytes, static_cast<std::uint32_t>(applicability.size()));
    for (auto &condition : applicability) {
        appendString(bytes, condition.json_pointer);
        std::vector<std::vector<unsigned char>> encoded_values;
        encoded_values.reserve(condition.active_values.size());
        for (const auto &value : condition.active_values) {
            auto &encoded = encoded_values.emplace_back();
            appendDiscriminantValue(encoded, value);
        }
        std::sort(encoded_values.begin(), encoded_values.end());
        appendU32(bytes,
                  static_cast<std::uint32_t>(encoded_values.size()));
        for (const auto &encoded : encoded_values) {
            bytes.insert(bytes.end(), encoded.begin(), encoded.end());
        }
    }
    // Preserve the v4 byte contract for every pre-v3 descriptor. Recursive
    // descriptors append an extension record; absence appends no new byte.
    if (descriptor.schema_node) {
        bytes.push_back(1U);
        appendSchemaNode(bytes, *descriptor.schema_node);
    }
}

std::string descriptorFingerprint(std::span<const BindableDescriptor> descriptors) {
    std::vector<unsigned char> bytes;
    appendString(bytes, "pelican.bindable.provider.v4");
    appendU32(bytes, static_cast<std::uint32_t>(descriptors.size()));
    for (const auto &descriptor : descriptors) {
        appendDescriptorRecord(bytes, descriptor);
    }
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

std::vector<unsigned char>
descriptorSortKey(const BindableDescriptor &descriptor) {
    std::vector<unsigned char> result;
    appendDescriptorRecord(result, descriptor);
    return result;
}

Schema::FieldType leafType(StructFieldType type);

std::vector<BindableDescriptor> codecDescriptors() {
    // The project-owned table is the shared engine/Studio source. The codec
    // pass below enriches only metadata that is owned by runtime schemas.
    auto result = builtinPrefabBindableDescriptors();
    const auto codec_defaults = componentCodecCanonicalDefaults();
    for (auto &descriptor : result) {
        if (descriptor.provider_name == "collider" &&
            descriptor.json_pointer == "/radius") {
            descriptor.canonical_default = f32(codec_defaults.collider_radius);
        }
    }

    // The codec-owned schema is the source of truth for type/range/enum
    // semantics. Defaults and conditional presence remain explicit above
    // because the runtime codec's omission rules are not represented by
    // StructFieldSchema.
    const auto pointerFor = [](std::string_view field_name) {
        std::string result{"/"};
        for (std::size_t index = 0; index < field_name.size(); ++index) {
            if (field_name[index] == '.') result.push_back('/');
            else if (field_name[index] == '[') {
                result.push_back('/');
                while (++index < field_name.size() && field_name[index] != ']') {
                    result.push_back(field_name[index]);
                }
            } else {
                result.push_back(field_name[index]);
            }
        }
        return result;
    };
    const auto convertRange = [](const StructFieldRange &range) {
        return std::visit([](const auto &value) -> Schema::FieldRange {
            using R = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<R, std::monostate>) return {};
            else if constexpr (std::same_as<R,
                                            std::pair<std::int64_t, std::int64_t>>)
                return Schema::SignedRange{value.first, value.second};
            else if constexpr (std::same_as<R,
                                            std::pair<std::uint64_t, std::uint64_t>>)
                return Schema::UnsignedRange{value.first, value.second};
            else return Schema::FloatingRange{value.first, value.second};
        }, range);
    };
    for (const auto &codec : componentCodecs()) {
        for (const auto &schema : codec.fieldSchema()) {
            const auto path = pointerFor(schema.name);
            for (auto &descriptor : result) {
                if (descriptor.provider_name != codec.name ||
                    descriptor.json_pointer != path) continue;
                descriptor.type = leafType(schema.type);
                descriptor.range = convertRange(schema.range);
                descriptor.enum_values.assign(
                    schema.enum_values,
                    schema.enum_values + schema.enum_value_count);
            }
        }
    }
    for (const auto &[source, target] :
         std::array<std::pair<std::string_view, std::string_view>, 8>{
             {{"/yfov", "/perspective/yfov"},
              {"/znear", "/perspective/znear"},
              {"/zfar", "/perspective/zfar"},
              {"/aspect", "/perspective/aspect"},
              {"/xmag", "/orthographic/xmag"},
              {"/ymag", "/orthographic/ymag"},
              {"/znear", "/orthographic/znear"},
              {"/zfar", "/orthographic/zfar"}}}) {
        const auto source_descriptor = std::find_if(
            result.begin(), result.end(), [&](const auto &descriptor) {
                return descriptor.provider_name == "camera" &&
                       descriptor.json_pointer == source;
            });
        const auto target_descriptor = std::find_if(
            result.begin(), result.end(), [&](const auto &descriptor) {
                return descriptor.provider_name == "camera" &&
                       descriptor.json_pointer == target;
            });
        if (source_descriptor != result.end() && target_descriptor != result.end()) {
            target_descriptor->type = source_descriptor->type;
            target_descriptor->range = source_descriptor->range;
            target_descriptor->enum_values = source_descriptor->enum_values;
        }
    }
    return result;
}

Schema::FieldType leafType(StructFieldType type) {
    static_assert(static_cast<unsigned>(StructFieldType::Enum) ==
                  static_cast<unsigned>(Schema::FieldType::Enum));
    return static_cast<Schema::FieldType>(type);
}

} // namespace

struct BindableProviderSnapshot::Data {
    std::uint64_t generation = 0;
    std::string fingerprint;
    std::vector<BindableDescriptor> descriptors;
    std::vector<BehaviorDependencyDescriptor> behavior_dependencies;
};

BindableProviderSnapshot::BindableProviderSnapshot() {
    auto data = std::make_shared<Data>();
    data->fingerprint = descriptorFingerprint(data->descriptors);
    data_ = std::move(data);
}

BindableProviderSnapshot::BindableProviderSnapshot(
    std::uint64_t generation, std::vector<BindableDescriptor> descriptors,
    std::vector<BehaviorDependencyDescriptor> behavior_dependencies) {
    if (generation == 0) throw std::invalid_argument("provider generation must be non-zero");
    std::sort(descriptors.begin(), descriptors.end(), [](const auto &lhs,
                                                         const auto &rhs) {
        return descriptorSortKey(lhs) < descriptorSortKey(rhs);
    });
    auto data = std::make_shared<Data>();
    data->generation = generation;
    data->descriptors = std::move(descriptors);
    std::sort(behavior_dependencies.begin(), behavior_dependencies.end(),
              [](const auto &lhs, const auto &rhs) {
                  return lhs.stable_name < rhs.stable_name;
              });
    const auto duplicate = std::adjacent_find(
        behavior_dependencies.begin(), behavior_dependencies.end(),
        [](const auto &lhs, const auto &rhs) {
            return lhs.stable_name == rhs.stable_name;
        });
    if (duplicate != behavior_dependencies.end()) {
        throw std::invalid_argument(
            "provider behavior dependency stable names must be unique");
    }
    data->behavior_dependencies = std::move(behavior_dependencies);
    data->fingerprint = descriptorFingerprint(data->descriptors);
    data_ = std::move(data);
}

std::uint64_t BindableProviderSnapshot::generation() const noexcept { return data_->generation; }
std::string_view BindableProviderSnapshot::fingerprint() const noexcept { return data_->fingerprint; }
std::span<const BindableDescriptor> BindableProviderSnapshot::descriptors() const noexcept { return data_->descriptors; }
std::span<const BehaviorDependencyDescriptor>
BindableProviderSnapshot::behaviorDependencies() const noexcept {
    return data_->behavior_dependencies;
}

const BindableDescriptor *BindableProviderSnapshot::find(
    std::string_view provider_name, std::string_view json_pointer) const noexcept {
    const auto found = std::find_if(
        data_->descriptors.begin(), data_->descriptors.end(),
        [&](const auto &descriptor) {
            return descriptor.bindable &&
                   descriptor.provider_name == provider_name &&
                   descriptor.json_pointer == json_pointer;
        });
    return found == data_->descriptors.end() ? nullptr : &*found;
}

bool BindableProviderSnapshot::containsPath(
    std::string_view provider_name, std::string_view json_pointer) const noexcept {
    return std::any_of(data_->descriptors.begin(), data_->descriptors.end(),
                       [&](const auto &descriptor) {
        return descriptor.bindable && descriptor.provider_name == provider_name &&
               descriptor.json_pointer == json_pointer;
    });
}

const BindableDescriptor *BindableProviderSnapshot::findActive(
    std::string_view provider_name, std::string_view json_pointer,
    const nlohmann::json &component) const {
    const auto found = std::find_if(
        data_->descriptors.begin(), data_->descriptors.end(),
        [&](const auto &descriptor) {
            return descriptor.bindable && descriptor.provider_name == provider_name &&
                   descriptor.json_pointer == json_pointer &&
                   active(descriptor, component);
        });
    return found == data_->descriptors.end() ? nullptr : &*found;
}

bool BindableProviderSnapshot::active(const BindableDescriptor &descriptor,
                                      const nlohmann::json &component) const {
    return prefabDescriptorActive(descriptor, component);
}

BindableProviderSnapshot buildProductionBindableProviderSnapshot(
    std::uint64_t generation) {
    auto descriptors = codecDescriptors();
    const auto &registrations = internal::getBehaviorRegisterer().registeredBehaviors();
    std::vector<BehaviorDependencyDescriptor> behavior_dependencies;
    behavior_dependencies.reserve(registrations.size());
    for (const auto &registration : registrations) {
        behavior_dependencies.push_back({
            .stable_name = registration.stable_name,
            .schema_version = registration.schema_version,
            .params_schema_fingerprint = registration.params_schema_fingerprint,
        });
        nlohmann::json defaults = nlohmann::json::object();
        if (registration.canonicalize_params != nullptr) {
            try {
                defaults = nlohmann::json::parse(
                    registration.canonicalize_params(nlohmann::json::object()));
            } catch (const std::exception &) {
                defaults = nlohmann::json::object();
            }
        }
        if (const auto *tree =
                internal::getBehaviorRegisterer().schemaFor(registration)) {
            for (const auto &schema : tree->children) {
                std::string escaped_name;
                for (const char ch : schema.name) {
                    if (ch == '~') escaped_name += "~0";
                    else if (ch == '/') escaped_name += "~1";
                    else escaped_name.push_back(ch);
                }
                auto def = schema.tag == Schema::NodeTag::Leaf &&
                                   schema.default_value
                               ? std::optional<OJson>{OJson::parse(
                                     schema.default_value->dump())}
                               : std::optional<OJson>{};
                descriptors.push_back({
                    .provider_name = "behavior:" + registration.stable_name,
                    .json_pointer = "/params/" + escaped_name,
                    .type = schema.tag == Schema::NodeTag::Leaf && schema.leaf
                                ? schema.leaf->type
                                : Schema::FieldType::String,
                    .semantic_kind = BindableSemanticKind::Value,
                    .bindable = schema.tag == Schema::NodeTag::Leaf,
                    .required = schema.presence ==
                                Schema::FieldPresence::Required,
                    .canonical_default = std::move(def),
                    .schema_node = schema,
                });
            }
            continue;
        }
        for (const auto &schema : registration.params_schema) {
            std::string escaped_name;
            for (const char ch : std::string_view{schema.name}) {
                if (ch == '~') escaped_name += "~0";
                else if (ch == '/') escaped_name += "~1";
                else escaped_name.push_back(ch);
            }
            auto pointer = "/params/" + escaped_name;
            auto def = defaults.find(std::string{schema.name});
            descriptors.push_back({
                .provider_name = "behavior:" + registration.stable_name,
                .json_pointer = std::move(pointer),
                .type = leafType(schema.type),
                .semantic_kind = BindableSemanticKind::Value,
                .required = false,
                .canonical_default = def == defaults.end()
                                         ? std::optional<OJson>{}
                                         : std::optional<OJson>{*def},
                .range = std::visit([](const auto &range) -> Schema::FieldRange {
                    using T = std::decay_t<decltype(range)>;
                    if constexpr (std::same_as<T, std::monostate>) return {};
                    else if constexpr (std::same_as<T, std::pair<std::int64_t, std::int64_t>>)
                        return Schema::SignedRange{range.first, range.second};
                    else if constexpr (std::same_as<T, std::pair<std::uint64_t, std::uint64_t>>)
                        return Schema::UnsignedRange{range.first, range.second};
                    else return Schema::FloatingRange{range.first, range.second};
                }, schema.range),
                .enum_values = std::vector<std::string>(
                    schema.enum_values,
                    schema.enum_values + schema.enum_value_count),
            });
        }
    }
    if (const auto *components =
            FastModuleContainer::tryGet<ComponentInfoManager>()) {
        for (const auto &component : components->registeredComponents()) {
            if (!component.token || !component.schema_v3) continue;
            descriptors.push_back({
                .provider_name = "component:" + component.name,
                .json_pointer = "",
                .type = Schema::FieldType::String,
                .semantic_kind = BindableSemanticKind::Value,
                .bindable = false,
                .required = false,
                .schema_node = *component.schema_v3,
            });
        }
    }
    return BindableProviderSnapshot{generation, std::move(descriptors),
                                    std::move(behavior_dependencies)};
}

} // namespace Pelican
