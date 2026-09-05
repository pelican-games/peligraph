#include "occurrenceidentity.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <picosha2.h>
#include <stdexcept>
#include <map>
#include <type_traits>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace Pelican {
namespace {

using Bytes = std::vector<std::uint8_t>;

void appendU32(Bytes &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void appendField(Bytes &out, std::span<const std::uint8_t> value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("occurrence identity field is too large");
    appendU32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void appendField(Bytes &out, std::string_view value) {
    appendField(out, {reinterpret_cast<const std::uint8_t *>(value.data()),
                      value.size()});
}

template <class T>
void appendScalarField(Bytes &out, T value) {
    static_assert(std::is_unsigned_v<T>);
    std::array<std::uint8_t, sizeof(T)> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
    appendField(out, bytes);
}

bool appendFloat(Bytes &out, float value) {
    if (!std::isfinite(value)) return false;
    if (value == 0.0F) value = 0.0F; // canonicalize -0
    appendU32(out, std::bit_cast<std::uint32_t>(value));
    return true;
}

template <glm::length_t N, class V>
bool appendFloatVector(Bytes &out, const std::vector<V> &values) {
    for (const auto &value : values)
        for (glm::length_t i = 0; i < N; ++i)
            if (!appendFloat(out, static_cast<float>(value[i]))) return false;
    return true;
}

void appendAttribute(Bytes &out, std::string_view semantic,
                     std::uint8_t type_tag, std::uint32_t element_count,
                     const Bytes &value) {
    appendField(out, semantic);
    appendScalarField(out, type_tag);
    appendScalarField(out, element_count);
    appendField(out, value);
}

OccurrenceDigest digest(const Bytes &bytes) {
    OccurrenceDigest result{};
    picosha2::hash256(bytes.begin(), bytes.end(), result.begin(), result.end());
    return result;
}

std::string hex128(const OccurrenceDigest &value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(32, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        result[i * 2] = hex[value[i] >> 4];
        result[i * 2 + 1] = hex[value[i] & 0xf];
    }
    return result;
}

Bytes variantBytes(const ImporterVariant &variant, bool &valid) {
    const auto raw = static_cast<std::uint8_t>(variant.kind);
    if (raw > static_cast<std::uint8_t>(ImporterVariantV1::vat_deformed)) {
        valid = false;
        return {};
    }
    Bytes result{raw};
    if (variant.kind == ImporterVariantV1::lod) result.push_back(variant.lod);
    return result;
}

bool validUtf8(std::string_view value) {
    const auto *p = reinterpret_cast<const unsigned char *>(value.data());
    std::size_t i = 0;
    while (i < value.size()) {
        const auto ch = p[i++];
        if (ch < 0x80) continue;
        std::size_t count = 0;
        std::uint32_t code = 0;
        if ((ch & 0xe0) == 0xc0) { count = 1; code = ch & 0x1f; }
        else if ((ch & 0xf0) == 0xe0) { count = 2; code = ch & 0x0f; }
        else if ((ch & 0xf8) == 0xf0) { count = 3; code = ch & 0x07; }
        else return false;
        if (i + count > value.size()) return false;
        for (std::size_t n = 0; n < count; ++n) {
            if ((p[i] & 0xc0) != 0x80) return false;
            code = (code << 6) | (p[i++] & 0x3f);
        }
        if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800) ||
            (count == 3 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

} // namespace

std::string_view occurrenceResolutionErrorCodeName(
    OccurrenceResolutionErrorCode code) noexcept {
    switch (code) {
    case OccurrenceResolutionErrorCode::absent: return "OccurrenceAbsent";
    case OccurrenceResolutionErrorCode::ambiguous: return "AmbiguousOccurrence";
    case OccurrenceResolutionErrorCode::not_ray_traced: return "OccurrenceNotRayTraced";
    case OccurrenceResolutionErrorCode::id_unrepresentable: return "OccurrenceIdUnrepresentable";
    }
    return "OccurrenceIdUnrepresentable";
}

std::optional<std::string> occurrenceNfcUtf8(
    std::string_view value, OccurrenceIdentityError &error) {
    if (!validUtf8(value)) {
        error.reason = "utf8";
        return std::nullopt;
    }
#ifdef _WIN32
    if (value.empty()) return std::string{};
    const int wide_count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (wide_count <= 0) { error.reason = "utf8"; return std::nullopt; }
    std::wstring wide(static_cast<std::size_t>(wide_count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), wide.data(), wide_count);
    const int normalized_count = NormalizeString(
        NormalizationC, wide.data(), wide_count, nullptr, 0);
    if (normalized_count <= 0) { error.reason = "nfc"; return std::nullopt; }
    std::wstring normalized(static_cast<std::size_t>(normalized_count), L'\0');
    NormalizeString(NormalizationC, wide.data(), wide_count,
                    normalized.data(), normalized_count);
    const int utf8_count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, normalized.data(), normalized_count,
        nullptr, 0, nullptr, nullptr);
    if (utf8_count <= 0) { error.reason = "utf8"; return std::nullopt; }
    std::string result(static_cast<std::size_t>(utf8_count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, normalized.data(),
                        normalized_count, result.data(), utf8_count,
                        nullptr, nullptr);
    return result;
#else
    // All shipped targets currently normalize through Windows Normaliz. UTF-8
    // validation remains deterministic for tooling-only non-Windows builds.
    return std::string{value};
#endif
}

OccurrenceIdentityResult occurrenceIdV1(
    const OccurrenceIdentityInput &input) {
    OccurrenceIdentityError error;
    if (!input.asset || input.asset->logical_name.empty()) {
        error.reason = "asset_key_missing";
        return {{}, error};
    }
    auto asset = occurrenceNfcUtf8(input.asset->logical_name, error);
    if (!asset) return {{}, error};

    Bytes node;
    if (input.node_explicit_id) {
        auto value = occurrenceNfcUtf8(*input.node_explicit_id, error);
        if (!value) return {{}, error};
        node.push_back(1);
        node.insert(node.end(), value->begin(), value->end());
    } else if (input.node_name_path_unique && !input.node_name_path.empty()) {
        node.push_back(2);
        for (const auto &part : input.node_name_path) {
            auto value = occurrenceNfcUtf8(part, error);
            if (!value || value->empty()) {
                if (error.reason.empty()) error.reason = "node_name_path";
                return {{}, error};
            }
            appendField(node, *value);
        }
    } else {
        node.push_back(3);
        node.insert(node.end(), input.node_content_digest.begin(),
                    input.node_content_digest.end());
    }

    Bytes primitive;
    Bytes content;
    if (input.primitive_explicit_id) {
        auto value = occurrenceNfcUtf8(*input.primitive_explicit_id, error);
        if (!value) return {{}, error};
        primitive.push_back(1);
        primitive.insert(primitive.end(), value->begin(), value->end());
    } else {
        primitive.push_back(3);
        primitive.insert(primitive.end(), input.primitive_content_digest.begin(),
                         input.primitive_content_digest.end());
        content.assign(input.primitive_content_digest.begin(),
                       input.primitive_content_digest.end());
    }

    bool variant_valid = true;
    const auto variant = variantBytes(input.variant, variant_valid);
    if (!variant_valid) {
        error.reason = "variant";
        return {{}, error};
    }

    Bytes encoded;
    static constexpr std::string_view prefix = "pelican.occurrence.v1";
    encoded.insert(encoded.end(), prefix.begin(), prefix.end());
    appendField(encoded, *asset);
    appendField(encoded, node);
    appendField(encoded, primitive);
    appendField(encoded, variant);
    appendField(encoded, content);
    return {hex128(digest(encoded)), std::nullopt};
}

OccurrenceDigest primitiveContentDigestV1(
    std::uint8_t topology_mode, const CommonPolygonVertData &data) {
    Bytes encoded;
    static constexpr std::string_view prefix = "pelican.primitive-content.v1";
    encoded.insert(encoded.end(), prefix.begin(), prefix.end());
    appendScalarField(encoded, topology_mode);
    appendScalarField(encoded, static_cast<std::uint8_t>(!data.indices.empty()));
    const auto index_count = data.indices.empty()
                                 ? static_cast<std::uint32_t>(data.pos.size())
                                 : static_cast<std::uint32_t>(data.indices.size());
    appendScalarField(encoded, index_count);
    Bytes index_bytes;
    index_bytes.reserve(data.indices.size() * 4);
    for (const auto index : data.indices) appendU32(index_bytes, index);
    appendField(encoded, index_bytes);

    struct Attribute { std::string_view name; std::uint8_t tag; std::uint32_t count; Bytes bytes; };
    std::vector<Attribute> attributes;
    const auto add = [&](std::string_view name, std::uint8_t tag,
                         const auto &values, glm::length_t width) {
        if (values.empty()) return;
        Bytes bytes;
        bool valid = true;
        for (const auto &value : values)
            for (glm::length_t i = 0; i < width; ++i)
                valid = appendFloat(bytes, static_cast<float>(value[i])) && valid;
        if (!valid) throw std::runtime_error("OccurrenceIdUnrepresentable: float");
        attributes.push_back({name, tag, static_cast<std::uint32_t>(values.size()), std::move(bytes)});
    };
    add("COLOR_0", 4, data.color, 4);
    add("NORMAL", 3, data.normal, 3);
    add("POSITION", 3, data.pos, 3);
    add("TANGENT", 4, data.tangent, 4);
    add("TEXCOORD_0", 2, data.texcoord, 2);
    std::sort(attributes.begin(), attributes.end(), [](const auto &a, const auto &b) {
        return a.name < b.name;
    });
    appendScalarField(encoded, static_cast<std::uint32_t>(attributes.size()));
    for (const auto &attribute : attributes)
        appendAttribute(encoded, attribute.name, attribute.tag,
                        attribute.count, attribute.bytes);

    appendScalarField(encoded, static_cast<std::uint32_t>(data.morph_targets.size()));
    for (const auto &target : data.morph_targets) {
        std::vector<Attribute> target_attributes;
        const auto add_target = [&](std::string_view name, const auto &values) {
            if (values.empty()) return;
            Bytes bytes;
            for (const auto &value : values)
                for (glm::length_t i = 0; i < 3; ++i)
                    if (!appendFloat(bytes, static_cast<float>(value[i])))
                        throw std::runtime_error("OccurrenceIdUnrepresentable: float");
            target_attributes.push_back({name, 3,
                static_cast<std::uint32_t>(values.size()), std::move(bytes)});
        };
        add_target("NORMAL", target.normal);
        add_target("POSITION", target.position);
        add_target("TANGENT", target.tangent);
        std::sort(target_attributes.begin(), target_attributes.end(),
                  [](const auto &a, const auto &b) { return a.name < b.name; });
        appendScalarField(encoded,
                          static_cast<std::uint32_t>(target_attributes.size()));
        for (const auto &attribute : target_attributes)
            appendAttribute(encoded, attribute.name, attribute.tag,
                            attribute.count, attribute.bytes);
    }

    if (!data.joint.empty() || !data.weight.empty()) {
        Bytes joints;
        for (const auto &value : data.joint)
            for (glm::length_t i = 0; i < 4; ++i)
                if (!appendFloat(joints, static_cast<float>(value[i])))
                    throw std::runtime_error(
                        "OccurrenceIdUnrepresentable: float");
        appendAttribute(encoded, "JOINTS_0", 4,
                        static_cast<std::uint32_t>(data.joint.size()), joints);
        Bytes weights;
        if (!appendFloatVector<4>(weights, data.weight))
            throw std::runtime_error("OccurrenceIdUnrepresentable: float");
        appendAttribute(encoded, "WEIGHTS_0", 4,
                        static_cast<std::uint32_t>(data.weight.size()), weights);
    }
    return digest(encoded);
}

OccurrenceDigest nodeContentDigestV1(
    std::span<const OccurrenceDigest> primitive_digests) {
    std::vector<OccurrenceDigest> ordered(primitive_digests.begin(),
                                          primitive_digests.end());
    std::sort(ordered.begin(), ordered.end());
    Bytes encoded;
    for (const auto &value : ordered) appendField(encoded, value);
    return digest(encoded);
}

std::string importerVariantV1Name(const ImporterVariant &variant) {
    switch (variant.kind) {
    case ImporterVariantV1::none: return "none";
    case ImporterVariantV1::vrm_split_both: return "vrm_split_both";
    case ImporterVariantV1::vrm_split_third_person_only: return "vrm_split_third_person_only";
    case ImporterVariantV1::lod: return "lod{" + std::to_string(variant.lod) + "}";
    case ImporterVariantV1::skinned: return "skinned";
    case ImporterVariantV1::morph_expanded: return "morph_expanded";
    case ImporterVariantV1::vat_deformed: return "vat_deformed";
    }
    return "unknown";
}

void detectOccurrenceIdCollisions(
    std::span<const std::shared_ptr<ModelOccurrenceIdentityInfo>> entries) {
    std::map<std::string, std::vector<ModelOccurrenceIdentityInfo *>> by_id;
    for (const auto &entry : entries) {
        if (!entry || entry->occurrence_id.empty()) continue;
        entry->ambiguous = false;
        by_id[entry->occurrence_id].push_back(entry.get());
    }
    for (auto &[_, matches] : by_id) {
        if (matches.size() < 2) continue;
        for (auto *match : matches) match->ambiguous = true;
    }
}

} // namespace Pelican
