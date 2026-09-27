#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {

// RS-1: the restart reason the render config editor reports when this is the
// only thing that kept a committed edit off the live engine.
inline constexpr const char *materialPhysicalContractChangedCode =
    "material_physical_contract_changed";

// RS-1: the parts of a live material's physical rendering contract a
// candidate pass can change (MaterialPipelineRenderingContract's fields).
inline constexpr const char *materialPhysicalContractColorFormats =
    "color_formats";
inline constexpr const char *materialPhysicalContractDepthFormat =
    "depth_format";
inline constexpr const char *materialPhysicalContractSampleCount =
    "sample_count";
inline constexpr const char *materialPhysicalContractLocalRead =
    "local_read";
inline constexpr const char *materialPhysicalContractAttachmentState =
    "attachment_state";

struct MaterialPhysicalContractChangeInfo {
    std::uint64_t material_id = 0;
    // The live material's name (may be empty).
    std::string material;
    // The candidate pass that would draw it with another contract.
    std::string pass;
    // Which parts differ, in the order above (materialPhysicalContract*).
    std::vector<std::string> detail;
};

// RS-1: a candidate render generation would draw a live material with a
// different physical rendering contract (color/depth formats, sample count,
// local-read mapping, attachment state). The live pipeline cannot be rebuilt
// in place, so the candidate is refused live; the engine takes it at its next
// start. MaterialGenerationPlanner::validateCompatibility throws this only
// after every other check of every live material has passed, so an editor
// that sees it knows it was the only live refusal.
class MaterialPhysicalContractChanged : public std::runtime_error {
  public:
    MaterialPhysicalContractChanged(std::string message,
                                    MaterialPhysicalContractChangeInfo info)
        : std::runtime_error(std::move(message)), info_(std::move(info)) {}

    [[nodiscard]] const MaterialPhysicalContractChangeInfo &
    info() const noexcept {
        return info_;
    }

  private:
    MaterialPhysicalContractChangeInfo info_;
};

} // namespace Pelican
