#pragma once

// IL-1 intro link: the pieces `pelican_cli intro-link bake` and `serve` share.
// Everything here is development tooling; PELICAN_WITH_INTRO_LINK=OFF drops
// these sources from pelican_cli and nothing in the engine refers to them.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican::DevCli::IntroLink {

// `code: detail` on one line, e.g. "intro_link.name_duplicate@1: ...".
class Error : public std::runtime_error {
  public:
    Error(std::string code, const std::string &detail);
    const std::string &code() const noexcept { return code_; }

  private:
    std::string code_;
};

// Wire interpolation values (0 linear / 1 step / 2 cubic).
enum : std::uint32_t { kLinear = 0, kStep = 1, kCubic = 2 };

struct Key {
    float time = 0;
    float value = 0;
    float in_tangent = 0;
    float out_tangent = 0;
    std::uint32_t interpolation = kLinear;
    bool operator==(const Key &) const = default;
};

// sizeof(pil_key) in the baked header: four floats and a uint32_t.
constexpr std::size_t kBakedKeyBytes = 20;

struct Track {
    std::string document; // project-relative, '/' separated
    std::vector<Key> keys;
};

struct ShaderSource {
    std::filesystem::path path;
    std::string relative; // project-relative, '/' separated
};

struct ShaderBinary {
    std::vector<std::uint32_t> words;
    std::string variant; // "Os" or "no-inline" (reported by bake; not in the header)
};

// <project> is a directory holding project.json, or project.json itself.
std::filesystem::path resolveProjectRoot(const std::string &argument);
std::filesystem::path resolveShadersDir(const std::filesystem::path &root,
                                        const std::string &argument);

bool isIdentifier(const std::string &name);
std::string relativeName(const std::filesystem::path &root, const std::filesystem::path &path);

// Every `value.curve` node of every pelican.graph document in passes/*.json
// (top-level nodes and definitions.*.nodes), keyed by node id.
std::map<std::string, Track> collectTracks(const std::filesystem::path &root);

// <shaders>/*.vert|*.frag|*.comp (not sub-directories), keyed by
// <stem>_<stage>.
std::map<std::string, ShaderSource> collectShaders(const std::filesystem::path &root,
                                                   const std::filesystem::path &shaders_dir);

// --glslc, then $VULKAN_SDK/Bin/glslc, then PATH.
std::filesystem::path findGlslc(const std::string &explicit_path);

struct CompileResult {
    bool ok = false;
    std::vector<std::uint32_t> words;
    std::string variant; // the build that was smaller: "Os" or "no-inline"
    std::string log;     // glslc stderr (and stdout on failure)
};
// The same rule for serve and bake: glslc --target-env=vulkan1.2 -Os
// -I <shaders> -I <shaders>/include <file> -o -, and with spirv-opt next to
// glslc also glslc -O0 + spirv-opt <spirv-opt's -Os list without inlining>;
// the smaller module is used (a tie keeps -Os).
CompileResult compileShader(const std::filesystem::path &glslc,
                            const std::filesystem::path &shaders_dir,
                            const ShaderSource &shader);

std::string renderBakedHeader(const std::map<std::string, Track> &tracks,
                              const std::map<std::string, ShaderBinary> &shaders);

struct ServeOptions {
    std::filesystem::path root;
    std::filesystem::path shaders_dir;
    std::filesystem::path glslc;
    int port = 47611;
    bool exit_after_client = false;
};
int runServe(const ServeOptions &options);

} // namespace Pelican::DevCli::IntroLink
