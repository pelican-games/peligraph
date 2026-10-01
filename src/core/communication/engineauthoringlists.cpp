#include "engineauthoringlists.hpp"

#include "../../project/graphmodule.hpp"
#include "../../project/materialoutput.hpp"
#include "../../project/renderpipeline.hpp"
#include "../../project/shaderresourceport.hpp"
#include "../fullscreenpass/passinputlayout.hpp"
#include "../fullscreenpass/shaderinputmismatchjson.hpp"
#include "../loader/engineresources.hpp"
#include "../renderingpass/materialpassattachments.hpp"
#include "../renderingpass/renderingpassjsonhelpers.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/shadercompiler.hpp"
#include "../shader/shaderresourceinterface.hpp"
#include "../shader/shaderreference.hpp"
#include "../shader/shaderreflection.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {

using OrderedJson = nlohmann::ordered_json;

OrderedJson materialPassContractsJson() {
    auto result = OrderedJson::array();
    for (const auto contract : materialPassContracts) {
        const auto name = std::string{materialPassContractName(contract)};
        // The sockets render.material_pass has for the contract alone, in
        // location order (the graph names its attachments so).
        const auto sockets = renderMaterialPassColorPorts(nlohmann::json{{"material_contract", name}});
        const auto shader_contract = materialPassShaderContract(contract);
        // Each table whose first format chooses that same table is one way
        // the pass composes; a table whose first format would choose the
        // other one never does.
        std::vector<std::vector<vk::Format>> combinations;
        for (const bool hdr : {false, true}) {
            auto formats = materialPassColorAttachmentFormatsFor(shader_contract, hdr);
            if (formats.empty() || materialPassUsesHdrColorFormats(formats.front()) != hdr) continue;
            if (std::find(combinations.begin(), combinations.end(), formats) == combinations.end())
                combinations.push_back(std::move(formats));
        }
        auto socket_entries = OrderedJson::array();
        for (std::size_t location = 0; location < sockets.size(); ++location) {
            auto formats = OrderedJson::array();
            std::string aspect;
            for (const auto &combination : combinations) {
                if (location >= combination.size()) continue;
                const auto format = formatToString(combination[location]);
                if (aspect.empty()) aspect = imageFormatAspectName(combination[location]);
                if (std::find(formats.begin(), formats.end(), format) == formats.end()) formats.push_back(format);
            }
            socket_entries.push_back({{"socket", sockets[location]}, {"aspect", aspect}, {"formats", formats}});
        }
        socket_entries.push_back({{"socket", "depth"},
                                  {"aspect", imageFormatAspectName(materialPassDepthAttachmentFormat)},
                                  {"formats", OrderedJson::array({formatToString(materialPassDepthAttachmentFormat)})}});
        auto combination_entries = OrderedJson::array();
        for (const auto &combination : combinations) {
            auto entry = OrderedJson::object();
            for (std::size_t location = 0; location < sockets.size() && location < combination.size(); ++location)
                entry[sockets[location]] = formatToString(combination[location]);
            entry["depth"] = formatToString(materialPassDepthAttachmentFormat);
            combination_entries.push_back(std::move(entry));
        }
        result.push_back({{"contract", name},
                          {"material_outputs", materialPassContractAcceptsMaterialOutputs(contract)},
                          {"sockets", std::move(socket_entries)},
                          {"combinations", std::move(combination_entries)}});
    }
    return result;
}

OrderedJson materialOutputFormatsJson() {
    const auto formats = imageFormats();
    auto color = OrderedJson::object();
    for (const auto type : materialOutputTypes) {
        auto accepted = OrderedJson::array();
        for (const auto &[name, format] : formats)
            if (imageFormatAspectName(format) == "color" &&
                materialPassFormatNumericClass(format) == materialOutputNumericClass(type))
                accepted.push_back(name);
        color[std::string{materialOutputTypeName(type)}] = std::move(accepted);
    }
    auto depth = OrderedJson::array();
    for (const auto &[name, format] : formats) {
        const auto aspect = imageFormatAspectName(format);
        if (aspect == "depth" || aspect == "depth_stencil") depth.push_back(name);
    }
    return {{"color", std::move(color)}, {"depth", std::move(depth)}};
}

namespace {

struct EngineShaderForms {
    std::string stem;
    ShaderStage stage = ShaderStage::vertex;
    bool source = false;
    bool spirv = false;
};

std::vector<std::uint32_t> spirvWords(std::string_view bytes) {
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), words.size() * sizeof(std::uint32_t));
    return words;
}

// What ShaderLibrary::loadFromStemReference would load: the GLSL source
// when the runtime compiler is built in, else the precompiled SPIR-V.
// Reflection reads that form; a source that does not compile on its own
// (one that includes a pass's generated ports) is read from its SPIR-V when
// there is one.
std::optional<ShaderReflection> reflectEngineShader(const EngineShaderForms &forms, std::string &from,
                                                    std::string &error) {
    // The candidates loadFromStemReference tries (shaderSourceCandidateReference).
    const auto source_id = forms.stem + std::string{shaderStageSourceExtension(forms.stage)};
    const auto spirv_id = source_id + ".spv";
#if PELICAN_RUNTIME_SHADER_COMPILER
    if (forms.source) {
        ShaderCompiler compiler;
        const auto name = "engine://" + source_id;
        const auto stage = inferShaderStageFromPath(source_id);
        const auto compiled = compiler.compileSource(*engineResource(source_id), *stage, name);
        if (compiled.ok) {
            from = "source";
            return reflect(compiled.spirv);
        }
        error = compiled.log;
    }
#endif
    if (forms.spirv) {
        const auto bytes = *engineResource(spirv_id);
        if (bytes.empty() || bytes.size() % sizeof(std::uint32_t) != 0) {
            error = "engine://" + spirv_id + " is not SPIR-V";
            return std::nullopt;
        }
        from = "spirv";
        error.clear();
        return reflect(spirvWords(bytes));
    }
    return std::nullopt;
}

OrderedJson engineShaderJson(const EngineShaderForms &forms) {
    OrderedJson entry{{"reference", "engine://" + forms.stem}, {"stage", shaderStageName(forms.stage)}};
    auto available = OrderedJson::array();
    if (forms.source) available.push_back("source");
    if (forms.spirv) available.push_back("spirv");
    entry["forms"] = std::move(available);
    entry["loads"] = PELICAN_RUNTIME_SHADER_COMPILER && forms.source ? "source" : "spirv";
    // A source that includes the resource-port header takes its ports from
    // the pass that runs it (the include is generated from the pass's
    // `ports`). On its own it compiles against the engine's empty include:
    // it reflects when its ports are optional (RS-11, bloom_composite), and
    // fails by the accessor's name when it calls one unconditionally.
    if (forms.source) {
        const auto source = *engineResource(forms.stem + std::string{shaderStageSourceExtension(forms.stage)});
        entry["resource_port_include"] =
            source.find("\"" + std::string{shaderResourcePortIncludeName} + "\"") != std::string_view::npos;
    }
    std::string from;
    std::string error;
    std::optional<ShaderReflection> reflection;
    try {
        reflection = reflectEngineShader(forms, from, error);
    } catch (const std::exception &failure) {
        error = failure.what();
    }
    if (!reflection) {
        entry["reflection_error"] = error;
        return entry;
    }
    entry["reflected_from"] = from;
    if (forms.stage == ShaderStage::vertex || forms.stage == ShaderStage::fragment) {
        // The pass-input set, as a fullscreen or raster pass binds it and
        // get_frame_plan's pass_inputs.declarations tell it.
        try {
            const auto layout = readPassInputLayout(*reflection);
            entry["pass_inputs"] = passInputDeclarationsJson(layout.declarations);
            entry["variadic_inputs"] = layout.variadic
                ? OrderedJson{{"first", layout.variadic->first}, {"max", layout.variadic->max}}
                : OrderedJson(nullptr);
        } catch (const std::exception &failure) {
            entry["pass_inputs_error"] = failure.what();
        }
    }
    // The resource ports: set 1's descriptors named pelican_resource_<port>,
    // where a compute or ray tracing task binds a port by its name (RS-5).
    auto ports = OrderedJson::array();
    for (const auto &binding : reflection->bindings) {
        if (binding.set != PELICAN_SET_PASS_INPUT || !binding.name.starts_with(shaderResourcePortVariablePrefix))
            continue;
        ports.push_back({{"port", binding.name.substr(shaderResourcePortVariablePrefix.size())},
                         {"binding", binding.binding},
                         {"type", passInputDescriptorTypeName(binding.type)}});
    }
    entry["resource_ports"] = std::move(ports);
    if (forms.stage == ShaderStage::fragment) {
        auto outputs = OrderedJson::array();
        for (const auto &output : reflection->fragment_outputs)
            outputs.push_back({{"location", output.location}, {"name", output.name}});
        entry["fragment_outputs"] = std::move(outputs);
    }
    if (forms.stage == ShaderStage::compute)
        entry["local_size"] = {reflection->local_size.x, reflection->local_size.y, reflection->local_size.z};
    return entry;
}

OrderedJson buildEngineShadersJson() {
    // (stem, stage) in the order stems and stages are listed.
    std::map<std::pair<std::string, std::size_t>, EngineShaderForms> found;
    for (const auto id : registeredEngineResourceIds()) {
        for (std::size_t index = 0; index < shaderStages.size(); ++index) {
            const auto stage = shaderStages[index];
            const auto source_suffix = std::string{shaderStageSourceExtension(stage)};
            const auto spirv_suffix = source_suffix + ".spv";
            const bool spirv = id.ends_with(spirv_suffix);
            if (!spirv && !id.ends_with(source_suffix)) continue;
            const auto stem = std::string{id.substr(0, id.size() - (spirv ? spirv_suffix : source_suffix).size())};
            if (stem.empty() || !engineResource(id)) continue;
            auto &forms = found[{stem, index}];
            forms.stem = stem;
            forms.stage = stage;
            (spirv ? forms.spirv : forms.source) = true;
        }
    }
    auto result = OrderedJson::array();
    for (const auto &[key, forms] : found) {
        (void)key;
        // A stem reference never loads a GLSL source without the compiler.
        if (!forms.spirv && !PELICAN_RUNTIME_SHADER_COMPILER) continue;
        result.push_back(engineShaderJson(forms));
    }
    return result;
}

} // namespace

const OrderedJson &engineShadersJson() {
    static const OrderedJson shaders = buildEngineShadersJson();
    return shaders;
}

} // namespace Pelican
