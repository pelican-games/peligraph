#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace Pelican {

// RTL-1b: a ray tracing refusal of the task parser (computetaskdefinitionjson)
// or of a layout name lookup (pelican.ray_group_name_unknown@1), with its code
// and the fields known where it was raised. what() is the sentence it always
// had ("<code>: compute task '<task>' ..."): the startup fatal line and
// Studio's one-sentence table read it, so only the type carries the fields.
//   code: the parser's own spelling, e.g. pelican.ray_tracing.hit_shader_missing@1
//   fields: among {task, ray_type, group, miss, stage, shader, keys, field,
//            name, kind}; the editor adds {document, node} for a task.
// The render config editor (a ticket's rejection, a reload's PB-1 problem)
// and renderPipelineRefusalJson (startup, hot reload) take the code and put
// the fields beside it.
class RayTracingRefusal : public std::runtime_error {
  public:
    RayTracingRefusal(std::string code, nlohmann::json fields, const std::string &message)
        : std::runtime_error(message), code_(std::move(code)), fields_(std::move(fields)) {}

    [[nodiscard]] const std::string &code() const noexcept { return code_; }
    [[nodiscard]] const nlohmann::json &fields() const noexcept { return fields_; }

  private:
    std::string code_;
    nlohmann::json fields_;
};

} // namespace Pelican
