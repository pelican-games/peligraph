#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {

// FO-2: a graphics pipeline refused because the pass it is built for does not
// declare what its shaders use -- not because a document could not be read.
// what() is the sentence it always had (the startup fatal line, a
// --strict-assets start and the tests read it); the type carries the rest.
//   requirement: what the pass would have to declare, e.g. "ray_query" (the
//                shaders read the scene acceleration structure; the pass has
//                no ray_query: true)
//   passes:      the passes that do not declare it, as the pass container
//                names them. Empty where the pipeline factory raised it (a
//                GraphicsPipelineDesc names no pass); MaterialContainer adds
//                the material passes the pipeline was for.
//   material:    the material whose pipeline it was (empty outside a
//                material's registration)
// RTL-1b's RayTracingRefusal is not reused: its catchers (the render config
// editor, renderPipelineRefusalJson) would rename every raster refusal --
// a fullscreen pass's, a debug draw's -- by a code it never had.
class PassContractRefusal : public std::runtime_error {
  public:
    PassContractRefusal(std::string requirement, std::vector<std::string> passes, std::string material,
                        const std::string &message)
        : std::runtime_error(message), requirement_(std::move(requirement)), passes_(std::move(passes)),
          material_(std::move(material)) {}

    [[nodiscard]] const std::string &requirement() const noexcept { return requirement_; }
    [[nodiscard]] const std::vector<std::string> &passes() const noexcept { return passes_; }
    // The first pass, or empty.
    [[nodiscard]] std::string pass() const { return passes_.empty() ? std::string{} : passes_.front(); }
    [[nodiscard]] const std::string &material() const noexcept { return material_; }
    // Why, in the words the pipeline factory used.
    [[nodiscard]] std::string reason() const { return what(); }

  private:
    std::string requirement_;
    std::vector<std::string> passes_;
    std::string material_;
};

} // namespace Pelican
