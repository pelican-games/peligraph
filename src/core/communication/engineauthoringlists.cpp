#include "engineauthoringlists.hpp"

#include "../../project/graphmodule.hpp"
#include "../../project/materialoutput.hpp"
#include "../../project/materialscreeninput.hpp"
#include "../../project/renderpipeline.hpp"
#include "../../project/shaderresourceport.hpp"
#include "../fullscreenpass/passinputlayout.hpp"
#include "../fullscreenpass/shaderinputmismatchjson.hpp"
#include "../loader/engineresources.hpp"
#include "../renderer/renderpolicyregistry.hpp"
#include "../renderingpass/graphtransformregistry.hpp"
#include "../renderingpass/materialpassattachments.hpp"
#include "../renderingpass/passimplementationregistry.hpp"
#include "../renderingpass/renderstrategyregistry.hpp"
#include "../renderingpass/subgraphreplacementregistry.hpp"
#include "../renderingpass/renderingpassjsonhelpers.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/shadercompiler.hpp"
#include "../shader/shaderresourceinterface.hpp"
#include "../shader/shaderreference.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../shader/shaderreflection.hpp"

#include <picosha2.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <system_error>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Pelican {

using OrderedJson = nlohmann::ordered_json;

OrderedJson materialPassContractsJson() {
    // MP-1: the input names a material pass binds, from the engine's lists:
    // {name, source_type}, the source_type the bound render target must have
    // (its semantic type). The parser takes the same names for every
    // contract, so each contract lists them all.
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto input_entries = [&](const auto &names) {
        auto entries = OrderedJson::array();
        for (const auto input : names) {
            const auto declared = makeBuiltinMaterialPassInputContract(types, input);
            entries.push_back({{"name", declared.name},
                               {"source_type", semanticTypeIdName(declared.source_type.semantic)}});
        }
        return entries;
    };
    const auto screen_inputs = input_entries(builtinMaterialScreenInputContractNames);
    const auto surface_resources = input_entries(featureOwnedMaterialPassInputContractNames);
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
                          {"combinations", std::move(combination_entries)},
                          {"screen_inputs", screen_inputs},
                          {"surface_resources", surface_resources}});
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

// EL-5: where one form (the GLSL source or the SPIR-V) of a stem's stage
// is, as the stem reference resolves it: a built-in resource or a file.
// `name` is what a diagnostic names (engine://<id>, or the candidate
// reference a project shader is read from).
struct ShaderFormLocation {
    std::string name;
    std::string engine_id;
    std::optional<std::filesystem::path> file;
};

// One stage of a stem reference: the forms it has, read the same way for
// the engine's shaders (EL-4b) and a project's (EL-5).
struct ShaderStageForms {
    std::string reference;
    ShaderStage stage = ShaderStage::vertex;
    std::optional<ShaderFormLocation> source;
    std::optional<ShaderFormLocation> spirv;
    // RTL-1b: generated includes a project source compiles with (the ray
    // group layout header).
    std::vector<std::pair<std::string, std::string>> virtual_includes;
};

std::vector<std::uint32_t> spirvWords(std::string_view bytes) {
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), words.size() * sizeof(std::uint32_t));
    return words;
}

std::string readFileBytes(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("cannot read " + path.generic_string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// A form's bytes: the built-in resource, or the file read from disk.
std::string readForm(const ShaderFormLocation &form) {
    if (form.file) return readFileBytes(*form.file);
    return std::string{*engineResource(form.engine_id)};
}

#if PELICAN_RUNTIME_SHADER_COMPILER
// A source form compiled as the shader library compiles it: a built-in
// source by its engine:// name, a file through compileFile (its directory
// and the engine's includes resolve its #include, as for a pass).
ShaderCompileResult compileForm(const ShaderFormLocation &form, ShaderCompiler &compiler,
                                const std::vector<std::pair<std::string, std::string>> &virtual_includes = {}) {
    if (form.file) {
        ShaderCompileOptions options;
        options.virtual_includes = virtual_includes;
        auto compiled = compiler.compileFile(*form.file, options);
        // The log names the file by its reference, not the machine's path.
        std::error_code ec;
        const auto physical = std::filesystem::weakly_canonical(*form.file, ec).string();
        for (auto at = compiled.log.find(physical); !ec && !physical.empty() && at != std::string::npos;
             at = compiled.log.find(physical, at + form.name.size()))
            compiled.log.replace(at, physical.size(), form.name);
        return compiled;
    }
    const auto stage = inferShaderStageFromPath(form.engine_id);
    return compiler.compileSource(*engineResource(form.engine_id), *stage, form.name);
}
#endif

// What ShaderLibrary::loadFromStemReference would load: the GLSL source
// when the runtime compiler is built in, else the precompiled SPIR-V.
// Reflection reads that form; a source that does not compile on its own
// (one that includes a pass's generated ports) is read from its SPIR-V when
// there is one. The compiler is the worker's own (EL-4c).
std::optional<ShaderReflection> reflectShaderForms(const ShaderStageForms &forms, ShaderCompiler &compiler,
                                                   std::string &from, std::string &error) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    if (forms.source) {
        const auto compiled = compileForm(*forms.source, compiler, forms.virtual_includes);
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
        const auto bytes = readForm(*forms.spirv);
        if (bytes.empty() || bytes.size() % sizeof(std::uint32_t) != 0) {
            error = forms.spirv->name + " is not SPIR-V";
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

// One stage's entry, get_engine_shaders' (EL-4b) and get_shader_interface's
// (EL-5) alike; only where the forms are read from differs.
OrderedJson shaderStageJson(const ShaderStageForms &forms, ShaderCompiler &compiler) {
    OrderedJson entry{{"reference", forms.reference}, {"stage", shaderStageName(forms.stage)}};
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
    std::string from;
    std::string error;
    std::optional<ShaderReflection> reflection;
    try {
        if (forms.source) {
            const auto source = readForm(*forms.source);
            entry["resource_port_include"] =
                source.find("\"" + std::string{shaderResourcePortIncludeName} + "\"") != std::string::npos;
        }
        reflection = reflectShaderForms(forms, compiler, from, error);
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
    std::map<std::pair<std::string, std::size_t>, ShaderStageForms> found;
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
            forms.reference = "engine://" + stem;
            forms.stage = stage;
            (spirv ? forms.spirv : forms.source) = ShaderFormLocation{.name = "engine://" + std::string{id},
                                                                      .engine_id = std::string{id}};
        }
    }
    auto result = OrderedJson::array();
    for (const auto &[key, forms] : found) {
        (void)key;
        // A stem reference never loads a GLSL source without the compiler.
        if (!forms.spirv && !PELICAN_RUNTIME_SHADER_COMPILER) continue;
        if (stop.load(std::memory_order_relaxed)) return std::nullopt;
        result.push_back(shaderStageJson(forms, compiler));
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

// EL-5: one get_shader_interface build: a stem's stages as the request
// found them.
struct ShaderInterfaceJob {
    std::string reference;
    std::uint64_t generation = 0;
    std::vector<ShaderStageForms> stages;
};

// EL-5: project shaders' interfaces, reflected on one worker thread of its
// own, one reference at a time in the order asked (a queue). Like EL-4c's
// worker it owns a detached compiler and reads only the disk (the stem's
// files and their includes) and the built-in resources; the frame loop's
// shader library and project cache are never touched. An answer is kept per
// reference with the hash of the stem's files it was built from.
class ShaderInterfaceReflection {
    struct Record {
        // The build the latest request asked for and the hash of the files
        // it reads; the answer and the build it came from (0: none yet).
        std::uint64_t wanted = 0;
        std::string wanted_key;
        std::uint64_t done = 0;
        OrderedJson answer;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::atomic_bool stop_requested{false};
    bool held_for_testing = false;
    std::uint64_t next_generation = 0;
    std::deque<ShaderInterfaceJob> queue;
    std::map<std::string, Record> records;

    void run() {
        auto compiler = ShaderCompiler::detached();
        for (;;) {
            ShaderInterfaceJob job;
            {
                std::unique_lock lock{mutex};
                wake.wait(lock, [&] { return stop_requested.load() || (!held_for_testing && !queue.empty()); });
                if (stop_requested.load()) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            OrderedJson answer;
            try {
                auto stages = OrderedJson::array();
                for (const auto &forms : job.stages) {
                    // Stopped: the unfinished build is dropped (stop()).
                    if (stop_requested.load(std::memory_order_relaxed)) return;
                    stages.push_back(shaderStageJson(forms, compiler));
                }
                answer = {{"status", "ready"}, {"reference", job.reference}, {"stages", std::move(stages)}};
            } catch (const std::exception &failure) {
                answer = {{"status", "failed"}, {"reference", job.reference}, {"error", failure.what()}};
            } catch (...) {
                answer = {{"status", "failed"}, {"reference", job.reference}, {"error", "shader reflection failed"}};
            }
            std::scoped_lock lock{mutex};
            // A request since (changed files or refresh) wants a newer build.
            auto &record = records[job.reference];
            if (record.wanted != job.generation) continue;
            record.done = job.generation;
            record.answer = std::move(answer);
        }
    }

  public:
    ~ShaderInterfaceReflection() { stop(); }

    // {status: building} until the build for these files is done, then its
    // answer. A changed key, or refresh, queues a new build (a queued one
    // for the same reference is replaced, not run twice).
    OrderedJson status(const std::string &reference, const std::string &key, std::vector<ShaderStageForms> stages,
                       bool refresh) {
        std::scoped_lock lock{mutex};
        auto &record = records[reference];
        if (!refresh && record.wanted != 0 && record.wanted_key == key) {
            if (record.done == record.wanted) return record.answer;
            return {{"status", "building"}};
        }
        record.wanted = ++next_generation;
        record.wanted_key = key;
        std::erase_if(queue, [&](const ShaderInterfaceJob &queued) { return queued.reference == reference; });
        queue.push_back({reference, record.wanted, std::move(stages)});
        if (!worker.joinable()) {
            stop_requested = false;
            worker = std::thread{[this] { run(); }};
        }
        wake.notify_all();
        return {{"status", "building"}};
    }

    // Raises stop (the worker leaves after the stage it is reflecting, or
    // while it waits) and joins it. Done answers are kept; queued and
    // unfinished builds are dropped, and the next request builds again.
    void stop() {
        std::thread running;
        {
            std::scoped_lock lock{mutex};
            stop_requested = true;
            running = std::move(worker);
        }
        wake.notify_all();
        if (running.joinable()) running.join();
        std::scoped_lock lock{mutex};
        stop_requested = false;
        queue.clear();
        for (auto &[reference, record] : records) {
            (void)reference;
            if (record.done == record.wanted) continue;
            record.wanted = record.done;
            record.wanted_key.clear();
        }
    }

    void reset() {
        stop();
        std::scoped_lock lock{mutex};
        records.clear();
    }

    void hold(bool held) {
        {
            std::scoped_lock lock{mutex};
            held_for_testing = held;
        }
        wake.notify_all();
    }
};

ShaderInterfaceReflection &shaderInterfaceReflection() {
    static ShaderInterfaceReflection reflection;
    return reflection;
}

OrderedJson shaderInterfaceFailure(std::string_view reference, std::string error) {
    return {{"status", "failed"}, {"reference", std::string{reference}}, {"error", std::move(error)}};
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

OrderedJson shaderInterfaceStatusJson(std::string_view reference, bool refresh, const ShaderReferenceResolver &resolve,
                                      std::vector<std::pair<std::string, std::string>> virtual_includes) {
    // An engine stem is get_engine_shaders' own entries (one reflection per
    // process; refresh does not apply).
    if (reference.starts_with("engine://")) {
        auto engine = engineShadersStatusJson();
        const auto status = engine.value("status", std::string{});
        if (status == "building") return engine;
        if (status != "ready") return shaderInterfaceFailure(reference, engine.value("error", std::string{}));
        auto stages = OrderedJson::array();
        for (auto &entry : engine.at("engine_shaders"))
            if (entry.at("reference").get<std::string>() == reference) stages.push_back(std::move(entry));
        if (stages.empty()) return shaderInterfaceFailure(reference, "shader_not_found");
        return {{"status", "ready"}, {"reference", std::string{reference}}, {"stages", std::move(stages)}};
    }
    if (!resolve) throw std::logic_error("get_shader_interface has no project to resolve " + std::string{reference});
    // The stage files a stem reference loads, by loadFromStemReference's
    // candidates and check; the hash of their bytes is the answer's key.
    std::vector<ShaderStageForms> stages;
    std::string hashed;
    for (const auto &[name, text] : virtual_includes)
        hashed += name + "\n" + picosha2::hash256_hex_string(text.begin(), text.end()) + "\n";
    for (const auto stage : shaderStages) {
        ShaderStageForms forms{.reference = std::string{reference}, .stage = stage,
                               .virtual_includes = virtual_includes};
        for (const bool spirv : {false, true}) {
            const auto candidate = shaderStemCandidateReference(reference, stage, spirv);
            std::optional<ResolvedRef> resolved;
            try {
                resolved = existingShaderStemCandidate(resolve(candidate), candidate);
            } catch (const std::exception &failure) {
                throw std::invalid_argument("get_shader_interface cannot resolve " + candidate + ": " +
                                            failure.what());
            }
            if (!resolved) continue;
            ShaderFormLocation location;
            if (const auto *path = std::get_if<std::filesystem::path>(&*resolved)) {
                location.name = candidate;
                location.file = *path;
            } else {
                location.engine_id = std::get<EngineResourceId>(*resolved).id;
                location.name = "engine://" + location.engine_id;
            }
            const auto bytes = readForm(location);
            hashed += candidate + '\n' + picosha2::hash256_hex_string(bytes.begin(), bytes.end()) + '\n';
            (spirv ? forms.spirv : forms.source) = std::move(location);
        }
        if (!forms.source && !forms.spirv) continue;
        // A stem reference never loads a GLSL source without the compiler.
        if (!forms.spirv && !PELICAN_RUNTIME_SHADER_COMPILER) continue;
        stages.push_back(std::move(forms));
    }
    if (stages.empty()) return shaderInterfaceFailure(reference, "shader_not_found");
    return shaderInterfaceReflection().status(std::string{reference}, picosha2::hash256_hex_string(hashed),
                                              std::move(stages), refresh);
}

void stopShaderInterfaceReflection() { shaderInterfaceReflection().stop(); }

void resetShaderInterfaceReflectionForTesting() { shaderInterfaceReflection().reset(); }

void holdShaderInterfaceReflectionForTesting(bool held) { shaderInterfaceReflection().hold(held); }

OrderedJson registeredProvidersJson() {
    const auto entries = [](const std::vector<RenderProviderListing> &providers) {
        auto result = OrderedJson::array();
        for (const auto &provider : providers)
            result.push_back({{"name", provider.name},
                              {"source", provider.engine_owned ? "engine" : "game"},
                              {"provider_version", provider.provider_version}});
        return result;
    };
    return {{"draw_sort", entries(renderPolicyRegistry().drawSortProviderListing())},
            {"pass_implementation", entries(passImplementationRegistry().providerListing())},
            {"region_replacements", entries(subgraphReplacementRegistry().providerListing())},
            {"graph_transforms", entries(graphTransformRegistry().providerListing())},
            {"render_strategy", entries(renderStrategyRegistry().providerListing())}};
}

} // namespace Pelican
