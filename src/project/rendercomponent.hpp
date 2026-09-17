#pragma once

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// P-2a-3: a shared render component is one materialized feature document plus
// the contract it needs from whatever project hosts it.  The two documents are
// written together under `user://components/<name>/` and read back the same
// way, so "copy the bytes" is never the whole operation.
inline constexpr std::string_view renderComponentSchema =
    "pelican.render_component";
inline constexpr int renderComponentVersion = 1;
inline constexpr std::string_view renderComponentFeatureFile = "feature.json";
inline constexpr std::string_view renderComponentManifestFile =
    "component.json";

// Carries the RPC rejection code, so the editor never has to re-derive a
// reason from a message.
class RenderComponentError : public std::runtime_error {
    std::string code_;

  public:
    RenderComponentError(std::string code, const std::string &message);
    const std::string &code() const noexcept { return code_; }
};

struct RenderComponent {
    std::string name;
    std::string feature_bytes;
    std::string manifest_bytes;
    std::string feature_digest;
    // Render targets the feature's passes read or write without declaring
    // them: the contract the host project has to satisfy.
    std::vector<std::string> required_render_targets;
};

// Names every render target the feature reads or writes that it does not
// declare itself.  `swapchain` is engine-owned and is reported like any other
// requirement; the importer treats it as always present.
std::vector<std::string> renderComponentRequiredTargets(
    const nlohmann::json &feature);

// Builds both documents for one feature.  Throws RenderComponentError with
// `render_component_engine_reference` for a shared engine feature and
// `render_component_project_reference` when any string value in the document
// still points into the exporting project.
RenderComponent buildRenderComponent(std::string_view feature_bytes,
                                     std::string_view reference,
                                     std::string_view project);

// Publishes the component by directory rename: an interrupted export leaves a
// `<destination>.tmp-<pid>` behind and never a half-written component.
void writeRenderComponent(const std::filesystem::path &destination,
                          const RenderComponent &component);

// Reads a component directory back, verifying the manifest envelope
// (`render_component_invalid`) and the feature digest
// (`render_component_digest_mismatch`).
RenderComponent readRenderComponent(const std::filesystem::path &source);

} // namespace Pelican
