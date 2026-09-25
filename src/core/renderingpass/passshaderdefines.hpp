#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// VI-1: why a pass's `shader.defines` entry is refused.
// The entry defines a name the engine defines itself when it compiles the
// pass (PELICAN_PASS_TEXTURE_INPUT_COUNT, PELICAN_VIEW_COUNT, a
// PELICAN_FEATURE_* or PELICAN_INPUT_* name, ...).
inline constexpr const char *passShaderDefineReserved = "reserved";
// Not a string of the form NAME or NAME=VALUE with NAME a C identifier.
inline constexpr const char *passShaderDefineMalformed = "malformed";
// The same NAME twice in one pass.
inline constexpr const char *passShaderDefineDuplicate = "duplicate";

struct PassShaderDefineErrorInfo {
    std::string pass;
    // The entry as authored ("NAME" or "NAME=VALUE"); for a non-string entry,
    // its JSON text.
    std::string define;
    // NAME; empty when the entry has none.
    std::string name;
    std::size_t index = 0;
    // passShaderDefine*.
    std::string reason;
};

// VI-1: a fullscreen / raster pass's `shader.defines` is refused. The
// render config editor reports it under render_pipeline_preflight_failed as
// error.pass_shader_define {pass, define, name, index, reason}.
class PassShaderDefineError : public std::runtime_error {
  public:
    explicit PassShaderDefineError(PassShaderDefineErrorInfo info);

    [[nodiscard]] const PassShaderDefineErrorInfo &info() const noexcept {
        return info_;
    }

  private:
    PassShaderDefineErrorInfo info_;
};

// Whether the engine defines `name` itself when it compiles a fullscreen or
// raster pass (so a pass may not).
bool isEngineReservedShaderDefine(std::string_view name);

// VI-1: reads `shader.defines` of a fullscreen / raster pass: an array of
// "NAME" / "NAME=VALUE" strings, as the root's `shader_defines`. Absent is
// empty. Throws PassShaderDefineError (or std::runtime_error when the member
// is not an array).
std::vector<std::string> parsePassShaderDefines(const nlohmann::json &shader,
                                                const std::string &pass_name);

// The name the engine defines to the number of texture inputs a pass
// supplies; pelican_view.glsl derives PELICAN_VARIADIC_INPUT_COUNT from it.
inline constexpr std::string_view passTextureInputCountDefine =
    "PELICAN_PASS_TEXTURE_INPUT_COUNT";

} // namespace Pelican
