#include "liveinputrecording.hpp"

#include "../appflow/enginetime.hpp"
#include "../launchconfig.hpp"
#include "../watch/problemlist.hpp"
#include "../../project/projectdocumenttransaction.hpp"

#if PELICAN_WITH_OSC
#include "../osc/oscinput.hpp"
#endif

#include <algorithm>
#include <iterator>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
constexpr std::uintmax_t max_file_bytes = 256ull * 1024ull * 1024ull;
constexpr std::size_t max_line_bytes = 1024ull * 1024ull;
constexpr std::size_t max_lines = 10000000;
constexpr std::size_t max_inputs = 4096;
constexpr std::size_t max_name_bytes = 256;

[[noreturn]] void invalid(std::size_t line, const std::string &cause) {
    throw std::runtime_error("live_input.recording_invalid@1: line " +
                             std::to_string(line) + ": " + cause);
}

class UniqueKeySax final : public nlohmann::json_sax<nlohmann::json> {
  public:
    bool start_object(std::size_t) override { objects_.emplace_back(); return true; }
    bool key(string_t &value) override {
        if (objects_.empty() || !objects_.back().insert(value).second) return false;
        return true;
    }
    bool end_object() override { objects_.pop_back(); return true; }
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t &) override { return true; }
    bool string(string_t &) override { return true; }
    bool start_array(std::size_t) override { return true; }
    bool end_array() override { return true; }
    bool binary(binary_t &) override { return true; }
    bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override {
        return false;
    }
  private:
    std::vector<std::set<std::string, std::less<>>> objects_;
};

Json parseUnique(std::string_view line, std::size_t line_number) {
    UniqueKeySax sax;
    if (!Json::sax_parse(line, &sax))
        invalid(line_number, "invalid JSON or duplicate object key");
    try {
        return Json::parse(line);
    } catch (const std::exception &error) {
        invalid(line_number, error.what());
    }
}

void onlyKeys(const Json &json, std::initializer_list<std::string_view> allowed,
              std::size_t line) {
    if (!json.is_object()) invalid(line, "line must be an object");
    for (const auto &[key, value] : json.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            invalid(line, "unknown key '" + key + "'");
    }
}

const Json &required(const Json &json, std::string_view key, std::size_t line) {
    const auto found = json.find(key);
    if (found == json.end()) invalid(line, "missing key '" + std::string{key} + "'");
    return *found;
}

std::uint64_t unsignedField(const Json &json, std::string_view key, std::size_t line) {
    const auto &value = required(json, key, line);
    if (!value.is_number_unsigned()) invalid(line, "field '" + std::string{key} + "' must be an unsigned integer");
    return value.get<std::uint64_t>();
}

double numberField(const Json &json, std::string_view key, std::size_t line) {
    const auto &value = required(json, key, line);
    if (!value.is_number()) invalid(line, "field '" + std::string{key} + "' must be numeric");
    const double result = value.get<double>();
    if (!std::isfinite(result)) invalid(line, "field '" + std::string{key} + "' must be finite");
    return result;
}

std::string stringField(const Json &json, std::string_view key, std::size_t line) {
    const auto &value = required(json, key, line);
    if (!value.is_string()) invalid(line, "field '" + std::string{key} + "' must be a string");
    return value.get<std::string>();
}

std::optional<double> optionalNumber(const Json &json, std::string_view key,
                                     std::size_t line) {
    const auto found = json.find(key);
    if (found == json.end()) return std::nullopt;
    if (!found->is_number() || !std::isfinite(found->get<double>()))
        invalid(line, "field '" + std::string{key} + "' must be finite and numeric");
    return found->get<double>();
}

double checkedValue(const LiveInputSpec &spec, const Json &value, std::size_t line) {
    if (!value.is_number()) invalid(line, "value for '" + spec.name + "' must be numeric");
    const double result = value.get<double>();
    if (!std::isfinite(result)) invalid(line, "value for '" + spec.name + "' must be finite");
    if (spec.type == LiveInputType::integer) {
        if (!value.is_number_integer() || std::trunc(result) != result ||
            result < std::numeric_limits<std::int32_t>::min() ||
            result > std::numeric_limits<std::int32_t>::max())
            invalid(line, "value for '" + spec.name + "' must be an int32 integer");
    } else if (spec.type == LiveInputType::boolean && result != 0.0 && result != 1.0) {
        invalid(line, "value for '" + spec.name + "' must be 0 or 1");
    }
    return result;
}

Json specJson(const LiveInputSpec &spec) {
    const auto scalar = [&](double value) -> Json {
        if (spec.type == LiveInputType::integer) return static_cast<std::int32_t>(value);
        if (spec.type == LiveInputType::boolean) return value == 0.0 ? 0 : 1;
        return value;
    };
    Json result{{"name", spec.name}, {"type", liveInputTypeName(spec.type)},
                {"default", scalar(spec.default_value)}};
    if (spec.minimum) result["min"] = scalar(*spec.minimum);
    if (spec.maximum) result["max"] = scalar(*spec.maximum);
    return result;
}

} // namespace

LiveInputRecording LiveInputRecording::fromJsonLines(std::string_view text) {
    if (text.size() > max_file_bytes) invalid(1, "file exceeds 256 MiB");
    LiveInputRecording result;
    std::map<std::string, std::size_t, std::less<>> indices;
    std::uint64_t previous_frame = 0;
    double previous_time = 0.0;
    bool saw_header = false;
    bool saw_snapshot = false;
    bool saw_end = false;
    std::size_t line_number = 0;
    std::size_t position = 0;
    while (position < text.size()) {
        if (++line_number > max_lines) invalid(line_number, "line count exceeds 10000000");
        const auto newline = text.find('\n', position);
        const auto length = (newline == std::string_view::npos ? text.size() : newline) - position;
        if (length > max_line_bytes) invalid(line_number, "line exceeds 1 MiB");
        auto line = text.substr(position, length);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        position = newline == std::string_view::npos ? text.size() : newline + 1;
        if (line.empty()) invalid(line_number, "empty line");
        const auto json = parseUnique(line, line_number);
        if (saw_end) invalid(line_number, "content follows the end line");
        if (!saw_header) {
            onlyKeys(json, {"schema", "version", "inputs", "clock", "fixed_step",
                            "start_frame", "start_time", "project_digest", "graph_digest"}, line_number);
            if (stringField(json, "schema", line_number) != "pelican.live_seq")
                invalid(line_number, "schema must be pelican.live_seq");
            if (unsignedField(json, "version", line_number) != 1)
                invalid(line_number, "version must be 1");
            const auto clock_name = stringField(json, "clock", line_number);
            if (clock_name == "fixed") result.clock.mode = LiveRecordingClockMode::fixed;
            else if (clock_name == "realtime") result.clock.mode = LiveRecordingClockMode::realtime;
            else invalid(line_number, "clock must be fixed or realtime");
            result.clock.fixed_step = numberField(json, "fixed_step", line_number);
            if (result.clock.fixed_step <= 0.0) invalid(line_number, "fixed_step must be positive");
            result.clock.start_frame = unsignedField(json, "start_frame", line_number);
            if (result.clock.start_frame == 0) invalid(line_number, "start_frame must be positive");
            result.clock.start_time = numberField(json, "start_time", line_number);
            if (const auto found = json.find("project_digest"); found != json.end()) {
                if (!found->is_string()) invalid(line_number, "project_digest must be a string");
                result.project_digest = found->get<std::string>();
            }
            if (const auto found = json.find("graph_digest"); found != json.end()) {
                if (!found->is_string()) invalid(line_number, "graph_digest must be a string");
                result.graph_digest = found->get<std::string>();
            }
            const auto &inputs = required(json, "inputs", line_number);
            if (!inputs.is_array()) invalid(line_number, "inputs must be an array");
            if (inputs.size() > max_inputs) invalid(line_number, "input count exceeds 4096");
            for (const auto &entry : inputs) {
                onlyKeys(entry, {"name", "type", "default", "min", "max"}, line_number);
                LiveInputSpec spec;
                spec.name = stringField(entry, "name", line_number);
                if (spec.name.empty() || spec.name.size() > max_name_bytes)
                    invalid(line_number, "input name length must be 1..256 bytes");
                if (!indices.emplace(spec.name, result.inputs.size()).second)
                    invalid(line_number, "duplicate input name '" + spec.name + "'");
                const auto type = stringField(entry, "type", line_number);
                if (type == "float") spec.type = LiveInputType::floating;
                else if (type == "int") spec.type = LiveInputType::integer;
                else if (type == "bool") spec.type = LiveInputType::boolean;
                else invalid(line_number, "unknown input type '" + type + "'");
                spec.default_value = checkedValue(spec, required(entry, "default", line_number), line_number);
                spec.minimum = optionalNumber(entry, "min", line_number);
                spec.maximum = optionalNumber(entry, "max", line_number);
                if (spec.type == LiveInputType::integer) {
                    for (const auto *bound : {&spec.minimum, &spec.maximum}) {
                        if (*bound && (std::trunc(**bound) != **bound ||
                            **bound < std::numeric_limits<std::int32_t>::min() ||
                            **bound > std::numeric_limits<std::int32_t>::max()))
                            invalid(line_number, "integer range must contain int32 integers");
                    }
                }
                if (spec.type == LiveInputType::boolean && (spec.minimum || spec.maximum))
                    invalid(line_number, "boolean input cannot have a range");
                if (spec.minimum && spec.maximum && *spec.minimum > *spec.maximum)
                    invalid(line_number, "minimum exceeds maximum");
                result.inputs.push_back(std::move(spec));
            }
            saw_header = true;
            continue;
        }
        if (!json.is_object()) invalid(line_number, "line must be an object");
        if (json.contains("end")) {
            onlyKeys(json, {"end", "frame", "time", "evaluations"}, line_number);
            if (!json.at("end").is_boolean() || !json.at("end").get<bool>())
                invalid(line_number, "end must be true");
            result.end.frame = unsignedField(json, "frame", line_number);
            result.end.time = numberField(json, "time", line_number);
            result.end.evaluations = unsignedField(json, "evaluations", line_number);
            if (saw_snapshot && result.end.evaluations == 0)
                invalid(line_number, "evaluations must be positive after evaluated frames");
            if (saw_snapshot && (result.end.frame < previous_frame || result.end.time < previous_time))
                invalid(line_number, "end frame or time precedes the last snapshot");
            if (!saw_snapshot && !result.inputs.empty()) invalid(line_number, "missing first complete snapshot");
            saw_end = true;
            continue;
        }
        onlyKeys(json, {"frame", "time", "values"}, line_number);
        LiveRecordingFrame frame;
        frame.frame = unsignedField(json, "frame", line_number);
        frame.time = numberField(json, "time", line_number);
        if (saw_snapshot && frame.frame <= previous_frame)
            invalid(line_number, "frame must be strictly increasing");
        if (saw_snapshot && frame.time < previous_time)
            invalid(line_number, "time must be non-decreasing");
        const auto &values = required(json, "values", line_number);
        if (!values.is_object() || values.empty()) invalid(line_number, "values must be a non-empty object");
        for (const auto &[name, value] : values.items()) {
            const auto found = indices.find(name);
            if (found == indices.end()) invalid(line_number, "unknown input name '" + name + "'");
            frame.values.emplace(name, checkedValue(result.inputs[found->second], value, line_number));
        }
        if (!saw_snapshot) {
            if (frame.frame != result.clock.start_frame || frame.time != result.clock.start_time)
                invalid(line_number, "first snapshot does not match the header clock");
            if (frame.values.size() != result.inputs.size())
                invalid(line_number, "first snapshot must contain every input");
            for (const auto &spec : result.inputs)
                if (!frame.values.contains(spec.name))
                    invalid(line_number, "first snapshot is missing '" + spec.name + "'");
            if (!result.inputs.empty() && frame.values.empty()) invalid(line_number, "values must not be empty");
        }
        previous_frame = frame.frame;
        previous_time = frame.time;
        result.frames.push_back(std::move(frame));
        saw_snapshot = true;
    }
    if (!saw_header) invalid(line_number ? line_number : 1, "missing header");
    if (!saw_end)
        throw std::runtime_error("live_input.recording_incomplete@1: recording has no end line");
    return result;
}

LiveInputRecording LiveInputRecording::loadFile(const std::filesystem::path &path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw std::runtime_error("live_input.recording_invalid@1: line 1: cannot read file: " + path.string());
    if (size > max_file_bytes) invalid(1, "file exceeds 256 MiB");
    std::ifstream input{path, std::ios::binary};
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (!input.good() && !input.eof()) invalid(1, "file read failed");
    return fromJsonLines(bytes.str());
}

const LiveInputSpec *LiveInputRecording::declaration(std::string_view name) const {
    const auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto &spec) {
        return spec.name == name;
    });
    return found == inputs.end() ? nullptr : &*found;
}

std::map<std::string, double, std::less<>> LiveInputRecording::valuesAtTime(double time) const {
    std::map<std::string, double, std::less<>> values;
    if (frames.empty()) return values;
    for (const auto &frame : frames) {
        if (frame.time > time) break;
        for (const auto &[name, value] : frame.values) values[name] = value;
    }
    for (const auto &[name, value] : frames.front().values)
        if (!values.contains(name)) values[name] = value;
    return values;
}

std::map<std::string, double, std::less<>> LiveInputRecording::valuesAtFrame(
    std::uint64_t relative_frame) const {
    std::map<std::string, double, std::less<>> values;
    if (frames.empty()) return values;
    for (const auto &frame : frames) {
        if (frame.frame > relative_frame) break;
        for (const auto &[name, value] : frame.values) values[name] = value;
    }
    for (const auto &[name, value] : frames.front().values)
        if (!values.contains(name)) values[name] = value;
    return values;
}

std::string LiveInputRecording::toJsonLines() const {
    if (frames.empty() && !inputs.empty())
        throw std::runtime_error("live_input.recording_incomplete@1: missing first snapshot");
    Json header{{"schema", "pelican.live_seq"}, {"version", 1},
                {"clock", clock.mode == LiveRecordingClockMode::fixed ? "fixed" : "realtime"},
                {"fixed_step", clock.fixed_step}, {"start_frame", clock.start_frame},
                {"start_time", clock.start_time}, {"inputs", Json::array()}};
    if (!project_digest.empty()) header["project_digest"] = project_digest;
    if (!graph_digest.empty()) header["graph_digest"] = graph_digest;
    for (const auto &spec : inputs) header["inputs"].push_back(specJson(spec));
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << header.dump() << '\n';
    for (const auto &frame : frames) {
        Json line{{"frame", frame.frame}, {"time", frame.time}, {"values", Json::object()}};
        for (const auto &[name, value] : frame.values) {
            const auto *spec = declaration(name);
            if (spec == nullptr) throw std::runtime_error("live_input.recording_invalid@1: unknown recorded input");
            if (spec->type == LiveInputType::integer)
                line["values"][name] = static_cast<std::int32_t>(value);
            else if (spec->type == LiveInputType::boolean)
                line["values"][name] = value == 0.0 ? 0 : 1;
            else line["values"][name] = value;
        }
        output << line.dump() << '\n';
    }
    output << Json{{"end", true}, {"frame", end.frame}, {"time", end.time},
                   {"evaluations", end.evaluations}}.dump() << '\n';
    return output.str();
}

LiveRecordingClock liveRecordingClock(const EngineTime &time, EngineTime::Mode mode) {
    LiveRecordingClock result;
    result.mode = mode == EngineTime::Mode::fixed_step
        ? LiveRecordingClockMode::fixed : LiveRecordingClockMode::realtime;
    result.fixed_step = time.stepDelta();
    result.start_frame = time.evaluationIndex() + 1;
    const bool advances = time.playState() == EngineTime::PlayState::playing ||
                          time.pendingSteps() > 0;
    result.start_time = time.now() + (advances ? time.stepDelta() : 0.0);
    result.time_set_revision = time.timeSetRevision();
    return result;
}

std::string liveRecordingProjectDigest(const std::filesystem::path &project_root) {
    std::ifstream input{project_root / "project.json", std::ios::binary};
    if (!input) return {};
    return projectDocumentDigest(std::string{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}});
}

LiveInputRecordingRuntime::LiveInputRecordingRuntime() {
    const auto *launch = FastModuleContainer::tryGet<EngineLaunchConfig>();
    if (launch != nullptr && launch->live_replay_path)
        launch_recording_ = std::make_shared<const LiveInputRecording>(
            LiveInputRecording::loadFile(*launch->live_replay_path));
}

LiveInputRecordingRuntime::~LiveInputRecordingRuntime() {
    if (recording_) {
        try { stopRecording(); } catch (...) { }
    }
}

void LiveInputRecordingRuntime::startRecording(
    const std::filesystem::path &path, std::vector<LiveInputSpec> inputs,
    const LiveRecordingClock &clock, std::string project_digest,
    std::string graph_digest) {
    if (replay_) throw std::runtime_error("live_input.record_while_replaying@1: cannot record during live replay");
    if (recording_) throw std::runtime_error("live_input.record_active@1: live recording is already active");
    if (inputs.size() > max_inputs) throw std::runtime_error("live_input.record_failed@1: more than 4096 inputs");
    for (const auto &spec : inputs)
        if (spec.name.size() > max_name_bytes)
            throw std::runtime_error("live_input.record_failed@1: input name exceeds 256 bytes");
    RecordingState state;
    state.path = path;
    state.clock = clock;
    state.inputs = std::move(inputs);
    state.output.open(path, std::ios::binary | std::ios::trunc);
    if (!state.output.is_open())
        throw std::runtime_error("live_input.record_failed@1: cannot open " + path.string());
    recording_.emplace(std::move(state));
    Json header{{"schema", "pelican.live_seq"}, {"version", 1},
                {"clock", clock.mode == LiveRecordingClockMode::fixed ? "fixed" : "realtime"},
                {"fixed_step", clock.fixed_step}, {"start_frame", clock.start_frame},
                {"start_time", clock.start_time}, {"inputs", Json::array()}};
    if (!project_digest.empty()) header["project_digest"] = project_digest;
    if (!graph_digest.empty()) header["graph_digest"] = graph_digest;
    for (const auto &spec : recording_->inputs) header["inputs"].push_back(specJson(spec));
    try {
        writeLine(header.dump());
        GET_MODULE(LiveInputs).setLiveRecording(true);
    } catch (...) {
        recording_.reset();
        throw;
    }
}

void LiveInputRecordingRuntime::stopRecording() {
    if (!recording_) throw std::runtime_error("live_input.record_inactive@1: no live recording is active");
    auto state = std::move(*recording_);
    recording_.reset();
    GET_MODULE(LiveInputs).setLiveRecording(false);
    std::ofstream &output = state.output;
    output << Json{{"end", true}, {"frame", state.last_frame},
                   {"time", state.last_time}, {"evaluations", state.status.evaluations}}.dump() << '\n';
    output.flush();
    if (!output) {
        reportProblem("live_input.record_failed@1", "error", "failed to write the live recording end line");
        return;
    }
}

void LiveInputRecordingRuntime::startReplay(
    std::shared_ptr<const LiveInputRecording> recording,
    const LiveRecordingClock &clock) {
    if (recording_) throw std::runtime_error("live_input.replay_active@1: cannot replay while recording live inputs");
    if (replay_) throw std::runtime_error("live_input.replay_active@1: live replay is already active");
    ReplayState state;
    state.recording = std::move(recording);
    state.clock = clock;
    state.frame_lookup = state.recording->clock.mode == LiveRecordingClockMode::fixed &&
        clock.mode == LiveRecordingClockMode::fixed &&
        state.recording->clock.fixed_step == clock.fixed_step &&
        state.recording->clock.start_time == clock.start_time;
    replay_.emplace(std::move(state));
    launch_recording_.reset();
    GET_MODULE(LiveInputs).beginLiveReplay();
#if PELICAN_WITH_OSC
    GET_MODULE(OscInput).discardScheduledForReplay();
#endif
}

void LiveInputRecordingRuntime::setReplayClock(const LiveRecordingClock &clock) {
    if (!replay_) throw std::runtime_error("live_input.replay_inactive@1: no live replay is active");
    replay_->clock = clock;
    replay_->frame_lookup = replay_->recording->clock.mode == LiveRecordingClockMode::fixed &&
        clock.mode == LiveRecordingClockMode::fixed &&
        replay_->recording->clock.fixed_step == clock.fixed_step &&
        replay_->recording->clock.start_time == clock.start_time;
}

void LiveInputRecordingRuntime::stopReplay() {
    if (!replay_) throw std::runtime_error("live_input.replay_inactive@1: no live replay is active");
    replay_.reset();
    GET_MODULE(LiveInputs).endLiveReplay();
}

void LiveInputRecordingRuntime::prepareFrame(const LiveRecordingClock &clock) {
    if (recording_ && recording_->clock.time_set_revision != clock.time_set_revision) {
        failRecording("engine time changed while recording");
    }
    if (replay_ && replay_->clock.time_set_revision != clock.time_set_revision) {
        replay_.reset();
        GET_MODULE(LiveInputs).endLiveReplay();
        reportProblem("live_input.replay_time_changed@1", "error",
                      "live replay stopped because engine time changed");
    }
}

std::map<std::string, double, std::less<>> LiveInputRecordingRuntime::replayValues(
    std::uint64_t frame, double time) const {
    if (!replay_) return {};
    if (replay_->frame_lookup) {
        const auto relative = frame >= replay_->clock.start_frame
            ? replay_->recording->clock.start_frame + frame - replay_->clock.start_frame
            : replay_->recording->clock.start_frame;
        return replay_->recording->valuesAtFrame(relative);
    }
    return replay_->recording->valuesAtTime(time);
}

void LiveInputRecordingRuntime::recordFrame(
    std::uint64_t frame, double time,
    const std::vector<LiveInputSnapshot> &snapshot) {
    if (!recording_) return;
    ++recording_->status.evaluations;
    recording_->last_frame = frame;
    recording_->last_time = time;
    Json values = Json::object();
    for (const auto &input : snapshot) {
        const auto previous = recording_->previous.find(input.spec.name);
        if (!recording_->first && previous != recording_->previous.end() && previous->second == input.value)
            continue;
        if (input.spec.type == LiveInputType::integer)
            values[input.spec.name] = static_cast<std::int32_t>(input.value);
        else if (input.spec.type == LiveInputType::boolean)
            values[input.spec.name] = input.value == 0.0 ? 0 : 1;
        else values[input.spec.name] = input.value;
        recording_->previous[input.spec.name] = input.value;
    }
    recording_->first = false;
    if (values.empty()) return;
    try {
        writeLine(Json{{"frame", frame}, {"time", time}, {"values", std::move(values)}}.dump());
    } catch (const std::exception &) {
        // Recording failure is a Problem and never a failed frame.
    }
}

std::optional<LiveRecordStatus> LiveInputRecordingRuntime::recordStatus() const {
    if (!recording_) return std::nullopt;
    return recording_->status;
}

std::uint64_t LiveInputRecordingRuntime::replayLines() const noexcept {
    return replay_ ? replay_->recording->frames.size() : 0;
}

void LiveInputRecordingRuntime::failRecordingAfterLinesForTesting(std::uint64_t lines) {
    if (recording_) recording_->fail_after = lines;
}

void LiveInputRecordingRuntime::failRecording(std::string_view detail) {
    if (!recording_) return;
    if (recording_->output.is_open()) recording_->output.close();
    recording_.reset();
    GET_MODULE(LiveInputs).setLiveRecording(false);
    reportProblem("live_input.record_failed@1", "error", detail);
}

void LiveInputRecordingRuntime::reportProblem(
    std::string_view code, std::string_view severity, std::string_view message) const {
    if (auto *problems = FastModuleContainer::tryGet<ProblemList>()) {
        problems->replace("live_input_recording", {Json{
            {"severity", std::string{severity}}, {"source", "live_input_recording"},
            {"code", std::string{code}}, {"message", std::string{message}},
            {"fields", Json::object()}}});
    }
}

void LiveInputRecordingRuntime::writeLine(const std::string &line) {
    if (!recording_) return;
    if (line.size() + 1 > max_line_bytes) {
        failRecording("recording line exceeds 1 MiB");
        throw std::runtime_error("live_input.record_failed@1: recording line exceeds 1 MiB");
    }
    if (recording_->status.lines + recording_->status.evaluations > max_lines) {
        failRecording("recording line count exceeds 10000000");
        throw std::runtime_error("live_input.record_failed@1: too many lines");
    }
    const auto started = std::chrono::steady_clock::now();
    auto &output = recording_->output;
    output << line << '\n';
    output.flush();
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    auto &status = recording_->status;
    status.total_write_ms += elapsed;
    status.max_write_ms = std::max(status.max_write_ms, elapsed);
    const bool injected_failure = recording_->fail_after != 0 && status.lines >= recording_->fail_after;
    if (!output || injected_failure) {
        const std::string detail = injected_failure ? "injected write failure" : "live recording write or flush failed";
        failRecording(detail);
        throw std::runtime_error("live_input.record_failed@1: " + detail);
    }
    ++status.lines;
}

} // namespace Pelican
