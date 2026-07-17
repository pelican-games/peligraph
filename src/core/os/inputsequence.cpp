#include "inputsequence.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

constexpr std::string_view inputSequenceSchema = "pelican.input_seq";
constexpr int inputSequenceVersion = 1;

void rejectPoseInputSequence(std::span<const std::string> pose_action_names,
                             std::string_view operation) {
    if (pose_action_names.empty()) return;
    throw std::runtime_error(std::string{operation} +
                             " does not support pose action '" + pose_action_names.front() +
                             "' in pelican.input_seq v1");
}

constexpr std::array<std::string_view, key_code_count> keyCodeNames{
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Space", "Enter", "Escape",
    "Tab", "Backspace", "LeftShift", "RightShift", "LeftControl", "RightControl",
    "LeftAlt", "RightAlt", "LeftSuper", "RightSuper", "F1", "F2", "F3", "F4",
    "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "MouseLeft",
    "MouseRight", "MouseMiddle", "MouseButton4", "MouseButton5", "MouseButton6",
    "MouseButton7", "MouseButton8",
};

static_assert(keyCodeNames.size() == key_code_count);

std::string pathString(const std::filesystem::path &path) {
    return path.string();
}

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error("input_seq file not found: " + pathString(path));
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::uint64_t requireUnsigned(const nlohmann::json &json, const char *field, std::size_t line) {
    const auto it = json.find(field);
    if (it == json.end() || !it->is_number_unsigned()) {
        throw std::runtime_error("input_seq line " + std::to_string(line) + " requires unsigned field '" +
                                 field + "'");
    }
    return it->get<std::uint64_t>();
}

float requireFloat(const nlohmann::json &json, const char *field, std::size_t line) {
    const auto it = json.find(field);
    if (it == json.end() || !it->is_number()) {
        throw std::runtime_error("input_seq line " + std::to_string(line) + " requires numeric field '" +
                                 field + "'");
    }
    const auto value = it->get<float>();
    if (!std::isfinite(value)) {
        throw std::runtime_error("input_seq line " + std::to_string(line) + " field '" + field +
                                 "' must be finite");
    }
    return value;
}

KeyCode parseKeyCode(const nlohmann::json &json, std::size_t line) {
    const auto it = json.find("code");
    if (it == json.end() || !it->is_string()) {
        throw std::runtime_error("input_seq line " + std::to_string(line) +
                                 " button event requires string field 'code'");
    }
    const auto name = it->get<std::string_view>();
    for (std::size_t i = 0; i < keyCodeNames.size(); ++i) {
        if (keyCodeNames[i] == name) {
            return static_cast<KeyCode>(i);
        }
    }
    throw std::runtime_error("input_seq line " + std::to_string(line) + " unknown button code: " +
                             std::string{name});
}

std::uint8_t parseGamepadSlot(const nlohmann::json &json, std::size_t line) {
    const auto slot = requireUnsigned(json, "pad", line);
    if (slot >= gamepad_slot_count) {
        throw std::runtime_error("input_seq line " + std::to_string(line) + " gamepad slot is out of range");
    }
    return static_cast<std::uint8_t>(slot);
}

std::string requireControlName(const nlohmann::json &json, std::size_t line, std::string_view kind) {
    const auto it = json.find("code");
    if (it == json.end() || !it->is_string()) {
        throw std::runtime_error("input_seq line " + std::to_string(line) + " " + std::string{kind} +
                                 " event requires string field 'code'");
    }
    return it->get<std::string>();
}

InputEvent parseEvent(const nlohmann::json &json, std::size_t line) {
    if (!json.is_object()) {
        throw std::runtime_error("input_seq line " + std::to_string(line) + " must be an object");
    }
    const auto sequence = requireUnsigned(json, "event_seq", line);
    const auto type_it = json.find("type");
    if (type_it == json.end() || !type_it->is_string()) {
        throw std::runtime_error("input_seq line " + std::to_string(line) + " requires string field 'type'");
    }

    InputEvent event;
    const auto type = type_it->get<std::string>();
    if (type == "button") {
        const auto pressed = json.find("pressed");
        if (pressed == json.end() || !pressed->is_boolean()) {
            throw std::runtime_error("input_seq line " + std::to_string(line) +
                                     " button event requires boolean field 'pressed'");
        }
        event = InputEvent::button(parseKeyCode(json, line), pressed->get<bool>());
    } else if (type == "cursor_move") {
        event = InputEvent::cursorMove(requireFloat(json, "x", line), requireFloat(json, "y", line));
    } else if (type == "axis") {
        event = InputEvent::axis(requireFloat(json, "x", line), requireFloat(json, "y", line));
    } else if (type == "scroll") {
        event = InputEvent::scroll(requireFloat(json, "x", line), requireFloat(json, "y", line));
    } else if (type == "character") {
        const auto codepoint = requireUnsigned(json, "codepoint", line);
        if (codepoint > 0x10ffffu) {
            throw std::runtime_error("input_seq line " + std::to_string(line) +
                                     " character codepoint is out of range");
        }
        event = InputEvent::character(static_cast<std::uint32_t>(codepoint));
    } else if (type == "pad_button") {
        const auto pressed = json.find("pressed");
        if (pressed == json.end() || !pressed->is_boolean()) {
            throw std::runtime_error("input_seq line " + std::to_string(line) +
                                     " pad_button event requires boolean field 'pressed'");
        }
        const auto name = requireControlName(json, line, "pad_button");
        const auto button = gamepadButtonFromName(name);
        if (!button) {
            throw std::runtime_error("input_seq line " + std::to_string(line) +
                                     " unknown gamepad button: " + name);
        }
        event = InputEvent::gamepadButton(parseGamepadSlot(json, line), *button, pressed->get<bool>());
    } else if (type == "pad_axis") {
        const auto name = requireControlName(json, line, "pad_axis");
        const auto axis = gamepadAxisFromName(name);
        if (!axis) {
            throw std::runtime_error("input_seq line " + std::to_string(line) +
                                     " unknown gamepad axis: " + name);
        }
        event = InputEvent::gamepadAxis(parseGamepadSlot(json, line), *axis,
                                        requireFloat(json, "value", line));
    } else {
        throw std::runtime_error("input_seq line " + std::to_string(line) + " unknown event type: " + type);
    }
    event.event_seq = sequence;
    return event;
}

nlohmann::json serializeEvent(const InputEvent &event) {
    nlohmann::json json{{"event_seq", event.event_seq}};
    switch (event.type) {
    case InputEvent::Type::button:
        if (!isValidKeyCode(event.code)) {
            throw std::runtime_error("cannot record input event with invalid button code");
        }
        json["type"] = "button";
        json["code"] = keyCodeNames[static_cast<std::size_t>(event.code)];
        json["pressed"] = event.pressed != 0;
        break;
    case InputEvent::Type::cursor_move:
        json["type"] = "cursor_move";
        json["x"] = event.mouse_x;
        json["y"] = event.mouse_y;
        break;
    case InputEvent::Type::axis:
        json["type"] = "axis";
        json["x"] = event.axis_x;
        json["y"] = event.axis_y;
        break;
    case InputEvent::Type::scroll:
        json["type"] = "scroll";
        json["x"] = event.axis_x;
        json["y"] = event.axis_y;
        break;
    case InputEvent::Type::character:
        json["type"] = "character";
        json["codepoint"] = event.codepoint;
        break;
    case InputEvent::Type::gamepad_button: {
        const auto name = gamepadButtonName(event.pad_button);
        if (event.gamepad >= gamepad_slot_count || name.empty()) {
            throw std::runtime_error("cannot record input event with invalid gamepad button");
        }
        json["type"] = "pad_button";
        json["pad"] = event.gamepad;
        json["code"] = name;
        json["pressed"] = event.pressed != 0;
        break;
    }
    case InputEvent::Type::gamepad_axis: {
        const auto name = gamepadAxisName(event.pad_axis);
        if (event.gamepad >= gamepad_slot_count || name.empty() || !std::isfinite(event.pad_value)) {
            throw std::runtime_error("cannot record input event with invalid gamepad axis");
        }
        json["type"] = "pad_axis";
        json["pad"] = event.gamepad;
        json["code"] = name;
        json["value"] = event.pad_value;
        break;
    }
    }
    return json;
}

} // namespace

InputSequence::InputSequence(double fps) : sequence_fps{fps} {
    if (!std::isfinite(fps) || fps <= 0.0) {
        throw std::runtime_error("input_seq fps must be positive and finite");
    }
}

InputSequence InputSequence::fromJsonLines(std::string_view json_lines) {
    std::istringstream stream{std::string{json_lines}};
    std::string line;
    std::size_t line_number = 0;
    bool header_read = false;
    bool saw_frame = false;
    std::optional<std::uint64_t> previous_event_seq;
    InputSequence parsed;

    while (std::getline(stream, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        nlohmann::json json;
        try {
            json = nlohmann::json::parse(line);
        } catch (const std::exception &error) {
            throw std::runtime_error("input_seq JSON parse failed on line " + std::to_string(line_number) +
                                     ": " + error.what());
        }

        if (!header_read) {
            if (!json.is_object() || json.value("schema", std::string{}) != inputSequenceSchema) {
                throw std::runtime_error("input_seq schema is not supported");
            }
            if (json.value("version", 0) != inputSequenceVersion) {
                throw std::runtime_error("input_seq version is not supported");
            }
            const auto fps_it = json.find("fps");
            if (fps_it == json.end() || !fps_it->is_number()) {
                throw std::runtime_error("input_seq header requires numeric fps");
            }
            parsed = InputSequence{fps_it->get<double>()};
            header_read = true;
            continue;
        }

        if (json.contains("frame")) {
            const auto frame = requireUnsigned(json, "frame", line_number);
            if (frame != parsed.frame_samples.size()) {
                throw std::runtime_error("input_seq frame markers must start at zero and be contiguous");
            }
            parsed.frame_samples.push_back(InputSequenceFrame{frame, {}});
            saw_frame = true;
            continue;
        }
        if (!saw_frame) {
            throw std::runtime_error("input_seq event appears before the first frame marker");
        }
        auto event = parseEvent(json, line_number);
        if (previous_event_seq && event.event_seq <= *previous_event_seq) {
            throw std::runtime_error("input_seq event_seq must be strictly increasing");
        }
        previous_event_seq = event.event_seq;
        parsed.frame_samples.back().events.push_back(event);
    }

    if (!header_read) {
        throw std::runtime_error("input_seq is empty");
    }
    if (parsed.frame_samples.empty()) {
        throw std::runtime_error("input_seq requires at least one frame marker");
    }
    return parsed;
}

InputSequence InputSequence::loadFile(const std::filesystem::path &path) {
    return fromJsonLines(readTextFile(path));
}

void InputSequence::appendFrame(std::span<const InputEvent> events) {
    InputSequenceFrame frame;
    frame.frame = frame_samples.size();
    frame.events.assign(events.begin(), events.end());
    frame_samples.push_back(std::move(frame));
}

std::string InputSequence::toJsonLines() const {
    std::ostringstream output;
    output << nlohmann::json{{"fps", sequence_fps},
                             {"generator", "pelican2"},
                             {"schema", inputSequenceSchema},
                             {"version", inputSequenceVersion}}
                  .dump()
           << '\n';
    for (const auto &frame : frame_samples) {
        output << nlohmann::json{{"frame", frame.frame}}.dump() << '\n';
        for (const auto &event : frame.events) {
            output << serializeEvent(event).dump() << '\n';
        }
    }
    return output.str();
}

void InputSequence::writeFile(const std::filesystem::path &path) const {
    if (path.empty()) {
        throw std::runtime_error("input record path must not be empty");
    }
    if (!path.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            throw std::runtime_error("failed to create input record directory: " +
                                     pathString(path.parent_path()) + " (" + error.message() + ")");
        }
    }
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file.is_open()) {
        throw std::runtime_error("failed to write input_seq file: " + pathString(path));
    }
    file << toJsonLines();
    if (!file) {
        throw std::runtime_error("failed while writing input_seq file: " + pathString(path));
    }
}

std::size_t InputSequenceRuntime::eventCount() const noexcept {
    std::size_t count = 0;
    for (const auto &frame : sequence.frames()) {
        count += frame.events.size();
    }
    return count;
}

InputSequenceRuntime::~InputSequenceRuntime() {
    if (isRecording()) {
        try {
            sequence.writeFile(output_path);
        } catch (...) {
        }
    }
}

void InputSequenceRuntime::startRecording(const std::filesystem::path &path, double fps,
                                          std::span<const std::string> pose_action_names) {
    if (isReplaying()) {
        throw std::runtime_error("start_input_record cannot run while start_input_replay is active");
    }
    if (isRecording()) {
        throw std::runtime_error("start_input_record is already active");
    }
    rejectPoseInputSequence(pose_action_names, "start_input_record");
    sequence = InputSequence{fps};
    output_path = path;
    mode = Mode::recording;
}

InputRecordResult InputSequenceRuntime::stopRecording() {
    if (!isRecording()) {
        throw std::runtime_error("stop_input_record requires an active start_input_record");
    }
    sequence.writeFile(output_path);
    InputRecordResult result{output_path, sequence.frames().size(), eventCount()};
    mode = Mode::idle;
    output_path.clear();
    return result;
}

void InputSequenceRuntime::startReplay(const std::filesystem::path &path,
                                       std::span<const std::string> pose_action_names) {
    if (isRecording()) {
        throw std::runtime_error("start_input_replay cannot run while start_input_record is active");
    }
    if (isReplaying()) {
        throw std::runtime_error("start_input_replay is already active");
    }
    rejectPoseInputSequence(pose_action_names, "start_input_replay");
    sequence = InputSequence::loadFile(path);
    output_path = path;
    replay_frame = 0;
    replay_sequence_offset.reset();
    mode = Mode::replaying;
}

void InputSequenceRuntime::stopReplay() {
    if (!isReplaying()) {
        throw std::runtime_error("stop_input_replay requires an active start_input_replay");
    }
    mode = Mode::idle;
    output_path.clear();
    replay_frame = 0;
    replay_sequence_offset.reset();
}

void InputSequenceRuntime::prepareFrame(InputState &input) {
    if (!isReplaying() || replayComplete()) {
        return;
    }
    const auto &source = sequence.frames()[replay_frame].events;
    std::vector<InputEvent> events{source.begin(), source.end()};
    if (!events.empty()) {
        if (!replay_sequence_offset) {
            const auto next = input.nextEventSequence();
            replay_sequence_offset = next > events.front().event_seq ? next - events.front().event_seq : 0;
        }
        for (auto &event : events) {
            if (event.event_seq > InputEvent::unassigned_sequence - 1 - *replay_sequence_offset) {
                throw std::runtime_error("input replay event sequence overflow");
            }
            event.event_seq += *replay_sequence_offset;
        }
        input.queueEvents(events);
    }
    ++replay_frame;
}

void InputSequenceRuntime::recordFrame(const FrameInput &frame_input) {
    if (isRecording()) {
        if (!frame_input.pose_samples.empty()) {
            throw std::runtime_error("start_input_record does not support pose action '" +
                                     frame_input.pose_samples.front().action_name +
                                     "' in pelican.input_seq v1");
        }
        sequence.appendFrame(frame_input.ordered_events);
    }
}

bool InputSequenceRuntime::replayComplete() const noexcept {
    return isReplaying() && replay_frame >= sequence.frames().size();
}

std::size_t InputSequenceRuntime::replayFrameCount() const noexcept {
    return isReplaying() ? sequence.frames().size() : 0;
}

} // namespace Pelican
