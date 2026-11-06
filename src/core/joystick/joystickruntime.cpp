#include "joystickruntime.hpp"

#include "../launchconfig.hpp"

#include <stdexcept>

namespace Pelican {
namespace {
using json = nlohmann::json;

[[noreturn]] void invalid(const std::string &what) {
    throw std::runtime_error("joystick.inject_invalid@1: " + what);
}

std::size_t slotParam(const json &params) {
    if (!params.contains("slot") || !params.at("slot").is_number_unsigned()) {
        invalid("slot must be an integer in 0..15");
    }
    const auto slot = params.at("slot").get<std::uint64_t>();
    if (slot >= joystick_slot_count) invalid("slot outside 0..15");
    return static_cast<std::size_t>(slot);
}

JoystickSample sampleParam(const json &params) {
    JoystickSample sample;
    for (const char *key : {"axes", "buttons", "hats"}) {
        if (!params.contains(key) || !params.at(key).is_array()) invalid(std::string{key} + " must be an array");
    }
    const auto &axes = params.at("axes");
    const auto &buttons = params.at("buttons");
    const auto &hats = params.at("hats");
    if (axes.size() > 64) invalid("more than 64 axes");
    if (buttons.size() > 256) invalid("more than 256 buttons");
    if (hats.size() > 16) invalid("more than 16 hats");
    for (const auto &axis : axes) {
        if (!axis.is_number()) invalid("axis is not a number");
        sample.axes.push_back(axis.get<float>());
    }
    for (const auto &button : buttons) {
        if (!button.is_number_integer() ||
            (button.get<int>() != 0 && button.get<int>() != 1)) {
            invalid("button is not 0 or 1");
        }
        sample.buttons.push_back(static_cast<std::uint8_t>(button.get<int>()));
    }
    for (const auto &hat : hats) {
        if (!hat.is_number_integer() || hat.get<int>() < 0 || hat.get<int>() > 15) {
            invalid("hat is outside 0..15");
        }
        sample.hats.push_back(static_cast<std::uint8_t>(hat.get<int>()));
    }
    return sample;
}
} // namespace

JoystickRuntime::JoystickRuntime() {
    const auto &launch = GET_MODULE(EngineLaunchConfig);
    if (launch.joystick_monitor) {
        if (launch.headless) {
            auto source = std::make_unique<InjectedJoystickSource>();
            injected_ = source.get();
            source_ = std::move(source);
        } else {
            source_ = makeGlfwJoystickSource();
        }
    }
    monitor_.emplace(source_.get());
}

void JoystickRuntime::pumpFrame() { monitor_->pumpFrame(JoystickClock::now()); }

nlohmann::json JoystickRuntime::devicesJson(bool with_values, const std::string &engine_instance_id) {
    const auto now = JoystickClock::now();
    if (with_values) monitor_->renewLease(now);
    return joystickDevicesJson(*monitor_, now, with_values, engine_instance_id);
}

nlohmann::json JoystickRuntime::injectJson(const nlohmann::json &params) {
    if (injected_ == nullptr) {
        throw std::runtime_error("joystick.inject_unavailable@1: injection needs --headless --joystick-monitor");
    }
    try {
        if (!params.contains("op") || !params.at("op").is_string()) invalid("op must be a string");
        const auto op = params.at("op").get<std::string>();
        const auto slot = slotParam(params);
        if (op == "connect") {
            for (const char *key : {"name", "guid"}) {
                if (!params.contains(key) || !params.at(key).is_string()) invalid(std::string{key} + " must be a string");
            }
            if (!params.contains("is_gamepad") || !params.at("is_gamepad").is_boolean()) {
                invalid("is_gamepad must be a bool");
            }
            injected_->connect(slot,
                JoystickIdentity{params.at("name").get<std::string>(),
                                 params.at("guid").get<std::string>(),
                                 params.at("is_gamepad").get<bool>()},
                sampleParam(params));
        } else if (op == "set") {
            injected_->set(slot, sampleParam(params));
        } else if (op == "disconnect") {
            injected_->disconnect(slot);
        } else {
            invalid("unknown op");
        }
    } catch (const std::invalid_argument &error) {
        invalid(error.what());
    } catch (const json::exception &error) {
        invalid(error.what());
    }
    return {{"ok", true}};
}
} // namespace Pelican
