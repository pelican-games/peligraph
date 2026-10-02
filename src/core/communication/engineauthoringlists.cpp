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
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
// there is one. The compiler is the worker's own (EL-4c).
std::optional<ShaderReflection> reflectEngineShader(const EngineShaderForms &forms, ShaderCompiler &compiler,
                                                    std::string &from, std::string &error) {
    // The candidates loadFromStemReference tries (shaderSourceCandidateReference).
    const auto source_id = forms.stem + std::string{shaderStageSourceExtension(forms.stage)};
    const auto spirv_id = source_id + ".spv";
#if PELICAN_RUNTIME_SHADER_COMPILER
    if (forms.source) {
        const auto name = "engine://" + source_id;
        const auto stage = inferShaderStageFromPath(source_id);
        const auto compiled = compiler.compileSource(*engineResource(source_id), *stage, name);
        if (compiled.ok) {
            from = "source";
            return reflect(compiled.spirv);
        }
        error = compiled.log;
    }
#else
    (void)compiler;
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

// EL-4c: a format's name as render_target_vocabulary's formats give it, if
// the vocabulary has the format.
std::optional<std::string> vocabularyFormatName(vk::Format format) {
    for (const auto &[name, listed] : imageFormats())
        if (listed == format) return name;
    return std::nullopt;
}

OrderedJson engineShaderJson(const EngineShaderForms &forms, ShaderCompiler &compiler) {
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
        reflection = reflectEngineShader(forms, compiler, from, error);
    } catch (const std::exception &failure) {
        error = failure.what();
    }
    if (!reflection) {
        entry["reflection_error"] = error;
        return entry;
    }
    entry["reflected_from"] = from;
    // LC-1: it declares pelican_loop.glsl's element and count.
    if (readsLoopIndex(*reflection)) entry["reads_loop_index"] = true;
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
        OrderedJson port{{"port", binding.name.substr(shaderResourcePortVariablePrefix.size())},
                         {"binding", binding.binding},
                         {"type", passInputDescriptorTypeName(binding.type)}};
        // EL-4c: a storage image's format, from its layout qualifier, by
        // the vocabulary's name; none when it declares none.
        if (binding.storage_image_format != vk::Format::eUndefined)
            if (const auto format = vocabularyFormatName(binding.storage_image_format)) port["format"] = *format;
        ports.push_back(std::move(port));
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

// Every engine shader's entry, or nothing when `stop` is raised before the
// last one: the worker looks at it between shaders.
std::optional<OrderedJson> buildEngineShadersJson(ShaderCompiler &compiler, const std::atomic_bool &stop) {
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
        if (stop.load(std::memory_order_relaxed)) return std::nullopt;
        result.push_back(engineShaderJson(forms, compiler));
    }
    return result;
}

std::atomic<std::size_t> engine_shaders_requests{0};

// EL-4c: the one reflection of the process. The worker owns its compiler
// (ShaderCompiler::detached: no module, no project cache, no StartupMetrics)
// and reads only the built-in resources and pure functions of them, so it
// shares nothing with the frame loop but this state, under the mutex.
class EngineShadersReflection {
    enum class State { idle, building, done };

    std::mutex mutex;
    std::condition_variable hold_changed;
    std::thread worker;
    std::atomic_bool stop_requested{false};
    bool held_for_testing = false;
    State state = State::idle;
    // The ready or failed answer, once done.
    OrderedJson answer;

    void run() {
        {
            std::unique_lock lock{mutex};
            hold_changed.wait(lock, [&] { return !held_for_testing || stop_requested.load(); });
        }
        OrderedJson result;
        try {
            auto compiler = ShaderCompiler::detached();
            auto shaders = buildEngineShadersJson(compiler, stop_requested);
            if (!shaders) {
                // Stopped: nothing is kept, and the next start begins again.
                std::scoped_lock lock{mutex};
                state = State::idle;
                return;
            }
            result = {{"status", "ready"}, {"engine_shaders", std::move(*shaders)}};
        } catch (const std::exception &failure) {
            result = {{"status", "failed"}, {"error", failure.what()}};
        } catch (...) {
            result = {{"status", "failed"}, {"error", "engine shader reflection failed"}};
        }
        std::scoped_lock lock{mutex};
        answer = std::move(result);
        state = State::done;
    }

    void startLocked() {
        if (state != State::idle) return;
        stop_requested = false;
        try {
            worker = std::thread{[this] { run(); }};
            state = State::building;
        } catch (const std::exception &failure) {
            answer = {{"status", "failed"}, {"error", failure.what()}};
            state = State::done;
        }
    }

  public:
    ~EngineShadersReflection() { stop(); }

    void start() {
        std::scoped_lock lock{mutex};
        startLocked();
    }

    OrderedJson status() {
        std::scoped_lock lock{mutex};
        startLocked();
        if (state == State::building) return {{"status", "building"}};
        return answer;
    }

    // Raises stop (the worker leaves after the shader it is reflecting, or
    // before its first when held) and joins it. A done answer is kept.
    void stop() {
        std::thread running;
        {
            std::scoped_lock lock{mutex};
            stop_requested = true;
            running = std::move(worker);
        }
        hold_changed.notify_all();
        if (running.joinable()) running.join();
        std::scoped_lock lock{mutex};
        stop_requested = false;
    }

    void reset() {
        stop();
        std::scoped_lock lock{mutex};
        state = State::idle;
        answer = OrderedJson{};
    }

    void hold(bool held) {
        {
            std::scoped_lock lock{mutex};
            held_for_testing = held;
        }
        hold_changed.notify_all();
    }
};

EngineShadersReflection &engineShadersReflection() {
    static EngineShadersReflection reflection;
    return reflection;
}

} // namespace

void startEngineShadersReflection() {
    engine_shaders_requests.fetch_add(1, std::memory_order_relaxed);
    engineShadersReflection().start();
}

OrderedJson engineShadersStatusJson() {
    engine_shaders_requests.fetch_add(1, std::memory_order_relaxed);
    return engineShadersReflection().status();
}

void stopEngineShadersReflection() { engineShadersReflection().stop(); }

void resetEngineShadersReflectionForTesting() { engineShadersReflection().reset(); }

void holdEngineShadersReflectionForTesting(bool held) { engineShadersReflection().hold(held); }

std::size_t engineShadersRequestCount() {
    return engine_shaders_requests.load(std::memory_order_relaxed);
}

} // namespace Pelican
