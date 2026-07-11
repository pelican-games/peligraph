#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

// CPU-visible descriptor remap record.  Descriptor type is intentionally part
// of the contract: one logical texture may be a sampled-image plus sampler in
// the experimental backend rather than a combined image sampler.
struct SpvLinkBinding {
    std::string logical_name;
    std::string descriptor_type;
    std::uint32_t original_set = 0;
    std::uint32_t original_binding = 0;
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
};

struct SpvLinkRequest {
    std::span<const std::uint32_t> template_module;
    std::span<const std::uint32_t> user_module;
    // Functions supplied by the user module and imported by the template.
    std::vector<std::string> user_exports;
    // Engine-library functions supplied by the template and imported by the
    // user module (the reverse side of the same ABI link).
    std::vector<std::string> template_exports;
    // Compiler options/defines and ABI/profile identities that affect a
    // variant even when a particular source does not reference the define.
    std::vector<std::string> cache_salts;
    std::vector<std::string> preserved_descriptor_names;
    std::uint32_t material_set = 2;
    std::uint32_t first_free_material_binding = 7;
};

struct SpvLinkResult {
    std::vector<std::uint32_t> spirv;
    std::vector<SpvLinkBinding> bindings;
    // Includes pinned tool revisions, compiler generator words, input hashes,
    // link options, and a digest of the complete key material.
    std::string cache_key;
};

SpvLinkResult linkSpirvModules(const SpvLinkRequest &request);

std::string spvLinkToolchainManifest();

} // namespace Pelican
