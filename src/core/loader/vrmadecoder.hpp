#pragma once

#include "../model/vrmaanimation.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace Pelican {

struct VrmaDecodeOptions {
    // If empty, the container name/path is used as the source URI.
    std::string source_uri;
    // Both fields are mandatory provenance supplied by the import boundary.
    std::string import_profile;
    std::string tool_version;
};

// Decodes an in-memory .vrma GLB and selects the normative first animation,
// identified as #animation/0. container_name is also the alias gate: it must
// have a .vrma extension.
std::shared_ptr<const VrmaClip>
decodeVrmaAnimation(std::span<const std::uint8_t> bytes,
                    std::string container_name,
                    VrmaDecodeOptions options);

// Reads bytes once, then hashes and decodes those exact bytes so provenance
// cannot describe content different from the decoded clip.
std::shared_ptr<const VrmaClip>
loadVrmaAnimation(const std::filesystem::path &path, VrmaDecodeOptions options);

} // namespace Pelican
