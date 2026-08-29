#include "prefabprovider.hpp"

#include "componentcodec.hpp"

#include "../userpublic/details/behavior/registerer.hpp"

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

std::string descriptorFingerprint(std::span<const BindableDescriptor> descriptors) {
    std::vector<unsigned char> bytes;
    appendString(bytes, "pelican.bindable.provider.v1");
    appendU32(bytes, static_cast<std::uint32_t>(descriptors.size()));
    for (const auto &descriptor : descriptors) {
        appendString(bytes, descriptor.provider_name);
        appendString(bytes, descriptor.json_pointer);
        bytes.push_back(static_cast<unsigned char>(descriptor.type));
        bytes.push_back(static_cast<unsigned char>(descriptor.semantic_kind));
        bytes.push_back(descriptor.bindable ? 1 : 0);
        bytes.push_back(descriptor.required ? 1 : 0);
        bytes.push_back(descriptor.canonical_default ? 1 : 0);
        if (descriptor.canonical_default) appendString(bytes, descriptor.canonical_default->dump());
        appendRange(bytes, descriptor.range);
        auto enum_values = descriptor.enum_values;
        std::sort(enum_values.begin(), enum_values.end());
        appendU32(bytes, static_cast<std::uint32_t>(enum_values.size()));
        for (const auto &value : enum_values) appendString(bytes, value);
        auto applicability = descriptor.applicability;
        std::sort(applicability.begin(), applicability.end(),
                  [](const auto &lhs, const auto &rhs) {
                      return lhs.json_pointer < rhs.json_pointer;
                  });
        appendU32(bytes, static_cast<std::uint32_t>(applicability.size()));
        for (auto &condition : applicability) {
            appendString(bytes, condition.json_pointer);
            std::sort(condition.active_values.begin(), condition.active_values.end(),
                      [](const auto &lhs, const auto &rhs) { return lhs.dump() < rhs.dump(); });
            appendU32(bytes, static_cast<std::uint32_t>(condition.active_values.size()));
            for (const auto &value : condition.active_values) appendString(bytes, value.dump());
        }
    }
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

std::string descriptorSortKey(const BindableDescriptor &descriptor) {
    OJson applicability = OJson::array();
    auto conditions = descriptor.applicability;
    std::sort(conditions.begin(), conditions.end(), [](const auto &lhs,
                                                        const auto &rhs) {
        return lhs.json_pointer < rhs.json_pointer;
    });
    for (auto &condition : conditions) {
        std::sort(condition.active_values.begin(), condition.active_values.end(),
                  [](const auto &lhs, const auto &rhs) {
                      return lhs.dump() < rhs.dump();
                  });
        applicability.push_back(
            OJson{{"path", condition.json_pointer},
                  {"values", condition.active_values}});
    }
    OJson enums = descriptor.enum_values;
    std::sort(enums.begin(), enums.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.dump() < rhs.dump();
    });
    std::vector<unsigned char> range_bytes;
    appendRange(range_bytes, descriptor.range);
    static constexpr char hex[] = "0123456789abcdef";
    std::string range_key;
    range_key.reserve(range_bytes.size() * 2U);
    for (const auto byte : range_bytes) {
        range_key.push_back(hex[byte >> 4U]);
        range_key.push_back(hex[byte & 0x0fU]);
    }
    return OJson{{"provider", descriptor.provider_name},
                 {"path", descriptor.json_pointer},
                 {"applicability", std::move(applicability)},
                 {"type", static_cast<unsigned>(descriptor.type)},
                 {"kind", static_cast<unsigned>(descriptor.semantic_kind)},
                 {"bindable", descriptor.bindable},
                 {"required", descriptor.required},
                 {"default", descriptor.canonical_default
                                 ? *descriptor.canonical_default
                                 : OJson(nullptr)},
                 {"range", std::move(range_key)},
                 {"enum", std::move(enums)}}
        .dump();
}

BindableDescriptor field(std::string provider, std::string path,
                         Schema::FieldType type, bool required,
                         std::optional<OJson> default_value = std::nullopt,
                         BindableSemanticKind kind = BindableSemanticKind::Value,
                         std::vector<BindableDiscriminant> applicability = {}) {
    return {.provider_name = std::move(provider),
            .json_pointer = std::move(path),
            .type = type,
            .semantic_kind = kind,
            .bindable = true,
            .required = required,
            .canonical_default = std::move(default_value),
            .applicability = std::move(applicability)};
}

Schema::FieldType leafType(StructFieldType type);

std::vector<BindableDescriptor> codecDescriptors() {
    using T = Schema::FieldType;
    using K = BindableSemanticKind;
    std::vector<BindableDescriptor> result;
    const auto add = [&](std::string provider, std::string path, T type,
                         bool required, std::optional<OJson> def = std::nullopt,
                         K kind = K::Value,
                         std::vector<BindableDiscriminant> applicability = {}) {
        result.push_back(field(std::move(provider), std::move(path), type,
                               required, std::move(def), kind,
                               std::move(applicability)));
    };
    const auto addDiscriminant = [&](std::string provider, std::string path,
                                     T type, bool required,
                                     std::optional<OJson> def = std::nullopt) {
        auto descriptor = field(std::move(provider), std::move(path), type,
                                required, std::move(def));
        descriptor.bindable = false;
        result.push_back(std::move(descriptor));
    };

    // All seven codec schemas are represented, including required/presence
    // semantics and canonical JSON defaults. Discriminants themselves are
    // intentionally absent: changing the active field set is not bindable.
    add("transform", "/pos", T::Vec3, false, OJson::array({0.0, 0.0, 0.0}));
    add("transform", "/rotation", T::Quat, false, OJson::array({0.0, 0.0, 0.0, 1.0}));
    add("transform", "/scale", T::Vec3, false, OJson::array({1.0, 1.0, 1.0}));
    add("simplemodelview", "/model", T::String, true, std::nullopt, K::Asset);

    const auto perspective = std::vector<BindableDiscriminant>{{"/type", {"perspective"}}};
    const auto orthographic = std::vector<BindableDiscriminant>{{"/type", {"orthographic"}}};
    addDiscriminant("camera", "/type", T::Enum, false, "perspective");
    add("camera", "/yfov", T::F32, false, f32(0.78539816339F), K::Value, perspective);
    add("camera", "/znear", T::F32, false, f32(0.1F));
    add("camera", "/zfar", T::F32, false, f32(1000.0F));
    add("camera", "/aspect", T::F32, false, f32(1.0F), K::Value, perspective);
    add("camera", "/xmag", T::F32, false, f32(1.0F), K::Value, orthographic);
    add("camera", "/ymag", T::F32, false, f32(1.0F), K::Value, orthographic);
    add("camera", "/sprite/pixel_perfect", T::Enum, false, "off");
    add("camera", "/sprite/sort", T::Enum, false, "z");
    const auto orbit = std::vector<BindableDiscriminant>{{"/controller/type", {"orbit"}}};
    const auto follow = std::vector<BindableDiscriminant>{{"/controller/type", {"follow"}}};
    const auto fly = std::vector<BindableDiscriminant>{{"/controller/type", {"fly"}}};
    addDiscriminant("camera", "/controller/type", T::String, true);
    add("camera", "/controller/target", T::String, false, std::nullopt,
        K::Object, orbit);
    add("camera", "/controller/target", T::String, true, std::nullopt,
        K::Object, follow);
    add("camera", "/controller/distance", T::F32, true, std::nullopt,
        K::Value, orbit);
    add("camera", "/controller/yaw", T::F32, false, f32(0.0F), K::Value, orbit);
    add("camera", "/controller/yaw_degrees", T::F32, false, f32(0.0F),
        K::Value, orbit);
    add("camera", "/controller/pitch", T::F32, false, f32(0.0F), K::Value, orbit);
    add("camera", "/controller/pitch_degrees", T::F32, false, f32(0.0F),
        K::Value, orbit);
    add("camera", "/controller/offset", T::Vec3, true, std::nullopt,
        K::Value, follow);
    add("camera", "/controller/speed", T::F32, true, std::nullopt,
        K::Value, fly);
    add("camera", "/controller/sensitivity", T::F32, false, f32(1.0F),
        K::Value, orbit);
    add("camera", "/controller/sensitivity", T::F32, true, std::nullopt,
        K::Value, fly);
    add("camera", "/controller/damping", T::F32, false, f32(0.0F));

    const auto point_or_spot = std::vector<BindableDiscriminant>{{"/type", {"point", "spot"}}};
    const auto spot = std::vector<BindableDiscriminant>{{"/type", {"spot"}}};
    addDiscriminant("light", "/type", T::Enum, true);
    add("light", "/position", T::Vec3, true, std::nullopt, K::Value,
        point_or_spot);
    add("light", "/direction", T::Vec3, true, std::nullopt, K::Value,
        {{"/type", {"directional", "spot"}}});
    add("light", "/intensity", T::F32, false, f32(1.0F));
    add("light", "/range", T::F32, false, f32(0.0F), K::Value, point_or_spot);
    add("light", "/color", T::Vec3, true);
    add("light", "/innerConeAngle", T::F32, false, f32(12.5F), K::Value, spot);
    add("light", "/outerConeAngle", T::F32, false, f32(17.5F), K::Value, spot);

    const auto sphere_or_capsule = std::vector<BindableDiscriminant>{{"/shape", {"sphere", "capsule"}}};
    const auto box = std::vector<BindableDiscriminant>{{"/shape", {"box"}}};
    const auto capsule = std::vector<BindableDiscriminant>{{"/shape", {"capsule"}}};
    addDiscriminant("collider", "/shape", T::Enum, false, "sphere");
    add("collider", "/pos", T::Vec3, false, OJson::array({0.0, 0.0, 0.0}));
    add("collider", "/rotation", T::Quat, false, OJson::array({0.0, 0.0, 0.0, 1.0}));
    add("collider", "/radius", T::F32, false, f32(0.5F), K::Value, sphere_or_capsule);
    add("collider", "/half_extents", T::Vec3, false, OJson::array({0.5, 0.5, 0.5}), K::Value, box);
    add("collider", "/half_height", T::F32, false, f32(0.5F), K::Value, capsule);
    add("collider", "/layer", T::U32, false, 1);
    add("collider", "/mask", T::U32, false, UINT32_MAX);
    add("collider", "/trigger", T::Bool, false, false);
    add("collider", "/one_way", T::Bool, false, false);

    add("animation", "/clip", T::String, true, std::nullopt, K::Asset);
    add("animation", "/speed", T::F64, false, 1.0);
    add("animation", "/loop", T::Bool, false, true);
    add("animation", "/start_time", T::F64, false, 0.0);

    add("sprite_view", "/texture", T::String, true, std::nullopt, K::Asset);
    add("sprite_view", "/size", T::Vec2, false, OJson::array({0.0, 0.0}));
    add("sprite_view", "/pivot", T::Vec2, false, OJson::array({0.5, 0.5}));
    add("sprite_view", "/color", T::Vec4, false, OJson::array({1.0, 1.0, 1.0, 1.0}));
    add("sprite_view", "/flip/0", T::Bool, false, false);
    add("sprite_view", "/flip/1", T::Bool, false, false);
    add("sprite_view", "/layer", T::I16, false, 0);
    add("sprite_view", "/billboard", T::Enum, false, "none");

    const auto cloneCameraPath = [&](std::string_view source,
                                     std::string target) {
        const auto current_size = result.size();
        for (std::size_t index = 0; index < current_size; ++index) {
            if (result[index].provider_name != "camera" ||
                result[index].json_pointer != source) continue;
            auto clone = result[index];
            clone.json_pointer = std::move(target);
            if (clone.json_pointer.starts_with("/perspective/")) {
                clone.applicability = {{"/type", {"perspective"}}};
            } else if (clone.json_pointer.starts_with("/orthographic/")) {
                clone.applicability = {{"/type", {"orthographic"}}};
            }
            result.push_back(std::move(clone));
            return;
        }
    };
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
        cloneCameraPath(source, std::string{target});
    }
    const auto controller_descriptors = result;
    for (const auto &descriptor : controller_descriptors) {
        if (descriptor.provider_name != "camera" ||
            !descriptor.json_pointer.starts_with("/controller/")) continue;
        auto clone = descriptor;
        clone.json_pointer = "/params" + clone.json_pointer;
        for (auto &condition : clone.applicability) {
            if (condition.json_pointer.starts_with("/controller/")) {
                condition.json_pointer = "/params" + condition.json_pointer;
            }
        }
        result.push_back(std::move(clone));
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
};

BindableProviderSnapshot::BindableProviderSnapshot()
    : data_{std::make_shared<const Data>()} {}

BindableProviderSnapshot::BindableProviderSnapshot(
    std::uint64_t generation, std::vector<BindableDescriptor> descriptors) {
    if (generation == 0) throw std::invalid_argument("provider generation must be non-zero");
    std::sort(descriptors.begin(), descriptors.end(), [](const auto &lhs,
                                                         const auto &rhs) {
        return descriptorSortKey(lhs) < descriptorSortKey(rhs);
    });
    auto data = std::make_shared<Data>();
    data->generation = generation;
    data->descriptors = std::move(descriptors);
    data->fingerprint = descriptorFingerprint(data->descriptors);
    data_ = std::move(data);
}

std::uint64_t BindableProviderSnapshot::generation() const noexcept { return data_->generation; }
std::string_view BindableProviderSnapshot::fingerprint() const noexcept { return data_->fingerprint; }
std::span<const BindableDescriptor> BindableProviderSnapshot::descriptors() const noexcept { return data_->descriptors; }

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
    for (const auto &condition : descriptor.applicability) {
        const auto pointer = nlohmann::json::json_pointer{condition.json_pointer};
        nlohmann::json implicit;
        const nlohmann::json *actual_pointer = nullptr;
        if (component.contains(pointer)) {
            actual_pointer = &component.at(pointer);
        } else if (condition.json_pointer == "/shape" &&
                   descriptor.provider_name == "collider") {
            implicit = "sphere";
        } else if (condition.json_pointer == "/type" &&
                   descriptor.provider_name == "camera") {
            implicit = component.contains("orthographic") ||
                               component.contains("xmag") ||
                               component.contains("ymag")
                           ? "orthographic"
                           : "perspective";
        } else if (condition.json_pointer == "/type" &&
                   descriptor.provider_name == "light") {
            implicit = "directional";
        } else {
            return false;
        }
        const nlohmann::json &actual = actual_pointer != nullptr
                                           ? *actual_pointer
                                           : implicit;
        if (std::none_of(condition.active_values.begin(), condition.active_values.end(),
                         [&](const auto &candidate) { return candidate == actual; })) {
            return false;
        }
    }
    return true;
}

BindableProviderSnapshot buildProductionBindableProviderSnapshot(
    std::uint64_t generation) {
    auto descriptors = codecDescriptors();
    const auto &registrations = internal::getBehaviorRegisterer().registeredBehaviors();
    for (const auto &registration : registrations) {
        nlohmann::json defaults = nlohmann::json::object();
        if (registration.canonicalize_params != nullptr) {
            try {
                defaults = nlohmann::json::parse(
                    registration.canonicalize_params(nlohmann::json::object()));
            } catch (const std::exception &) {
                defaults = nlohmann::json::object();
            }
        }
        for (const auto &schema : registration.params_schema) {
            auto pointer = "/params/" + std::string{schema.name};
            std::replace(pointer.begin(), pointer.end(), '.', '/');
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
    return BindableProviderSnapshot{generation, std::move(descriptors)};
}

bool prefabParameterMatchesDescriptor(
    const PrefabParameterDeclaration &parameter,
    const BindableDescriptor &descriptor) noexcept {
    if (parameter.kind == PrefabParameterKind::Value) {
        return parameter.value_schema.type == descriptor.type &&
               descriptor.semantic_kind == BindableSemanticKind::Value;
    }
    if (descriptor.type != Schema::FieldType::String) return false;
    if (parameter.kind == PrefabParameterKind::Asset) {
        return descriptor.semantic_kind == BindableSemanticKind::Asset ||
               descriptor.semantic_kind == BindableSemanticKind::Value;
    }
    return descriptor.semantic_kind == BindableSemanticKind::Object ||
           descriptor.semantic_kind == BindableSemanticKind::Value;
}

} // namespace Pelican
