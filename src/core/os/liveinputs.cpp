#include "liveinputs.hpp"

#include "../loader/basicconfig.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

constexpr std::string_view declaration_schema = "pelican.live_inputs";
constexpr std::int64_t declaration_version = 1;

[[noreturn]] void refuse(std::string_view code, std::string_view detail) {
    throw std::runtime_error(std::string{code} + ": " + std::string{detail});
}

bool validName(std::string_view name) {
    if (name.empty()) return false;
    const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (!lower(name.front())) return false;
    for (const char c : name.substr(1))
        if (!lower(c) && !digit(c) && c != '_') return false;
    return true;
}

LiveInputType parseType(const nlohmann::json &value, std::string_view name) {
    if (!value.is_string())
        refuse("live_input.type@1", "input '" + std::string{name} + "' has no string type");
    const auto type = value.get<std::string>();
    if (type == "trigger")
        refuse("live_input.trigger_unsupported@1",
               "input '" + std::string{name} + "' is a trigger; triggers are not live inputs");
    if (type == "float") return LiveInputType::floating;
    if (type == "int") return LiveInputType::integer;
    if (type == "bool") return LiveInputType::boolean;
    refuse("live_input.type@1", "input '" + std::string{name} + "' has unknown type '" + type + "'");
}

bool isIntegerJson(const nlohmann::json &value) {
    // nlohmann parses a non-negative literal as number_unsigned. Both are
    // JSON integers; readInt32 applies the declared int32 range.
    return value.is_number_integer();
}

bool isVersionOne(const nlohmann::json &value) {
    if (!isIntegerJson(value)) return false;
    // Check the stored width instead of converting an arbitrary unsigned
    // literal to int64. A malformed version must be a named refusal, not a
    // conversion exception from the JSON library.
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>() == static_cast<std::uint64_t>(declaration_version);
    return value.get<std::int64_t>() == declaration_version;
}

std::int64_t readInt32(const nlohmann::json &value, std::string_view name,
                       std::string_view field, std::string_view code) {
    if (!isIntegerJson(value))
        refuse(code, "input '" + std::string{name} + "' field '" + std::string{field} +
                         "' must be an integer");
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
            return static_cast<std::int64_t>(unsigned_value);
        refuse(code, "input '" + std::string{name} + "' field '" + std::string{field} +
                         "' is outside int32");
    }
    const auto integer = value.get<std::int64_t>();
    if (integer < std::numeric_limits<std::int32_t>::min() ||
        integer > std::numeric_limits<std::int32_t>::max())
        refuse(code, "input '" + std::string{name} + "' field '" + std::string{field} +
                         "' is outside int32");
    return integer;
}

std::vector<LiveInputSpec> parseDeclaration(std::string_view text) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(text);
    } catch (const std::exception &error) {
        refuse("live_input.schema@1", std::string{"not valid JSON: "} + error.what());
    }
    if (!document.is_object())
        refuse("live_input.schema@1", "document root must be an object");
    if (!document.contains("schema") || !document["schema"].is_string() ||
        document["schema"].get<std::string>() != declaration_schema)
        refuse("live_input.schema@1",
               "document schema must be '" + std::string{declaration_schema} + "'");
    if (!document.contains("version") || !isVersionOne(document["version"]))
        refuse("live_input.version@1", "document version must be 1");
    if (!document.contains("inputs") || !document["inputs"].is_array())
        refuse("live_input.inputs@1", "document inputs must be an array");

    std::vector<LiveInputSpec> specs;
    std::map<std::string, bool, std::less<>> seen;
    for (const auto &entry : document["inputs"]) {
        if (!entry.is_object())
            refuse("live_input.declaration@1", "every input must be an object");
        if (!entry.contains("name") || !entry["name"].is_string())
            refuse("live_input.name@1", "every input needs a string name");
        const auto name = entry["name"].get<std::string>();
        if (!validName(name))
            refuse("live_input.name@1",
                   "input name '" + name + "' must match [a-z][a-z0-9_]*");
        if (seen.contains(name))
            refuse("live_input.duplicate@1", "input '" + name + "' is declared twice");
        seen.emplace(name, true);
        if (!entry.contains("type"))
            refuse("live_input.type@1", "input '" + name + "' needs a type");
        const auto type = parseType(entry["type"], name);

        LiveInputSpec spec{name, type};
        if (!entry.contains("default"))
            refuse("live_input.default_required@1", "input '" + name + "' needs a default");
        const auto &default_value = entry["default"];
        switch (type) {
        case LiveInputType::floating:
            if (!default_value.is_number())
                refuse("live_input.default_type@1",
                       "float input '" + name + "' needs a numeric default");
            spec.default_value = default_value.get<double>();
            if (!std::isfinite(spec.default_value))
                refuse("live_input.default_type@1",
                       "float input '" + name + "' needs a finite default");
            break;
        case LiveInputType::integer:
            spec.default_value = static_cast<double>(readInt32(
                default_value, name, "default", "live_input.default_type@1"));
            break;
        case LiveInputType::boolean:
            if (!default_value.is_boolean())
                refuse("live_input.default_type@1",
                       "bool input '" + name + "' needs a boolean default");
            spec.default_value = default_value.get<bool>() ? 1.0 : 0.0;
            break;
        }

        const bool has_min = entry.contains("min");
        const bool has_max = entry.contains("max");
        if (type == LiveInputType::boolean && (has_min || has_max))
            refuse("live_input.range@1", "bool input '" + name + "' cannot have a range");
        if (has_min) {
            const auto &minimum = entry["min"];
            if (type == LiveInputType::floating) {
                if (!minimum.is_number() || !std::isfinite(minimum.get<double>()))
                    refuse("live_input.range@1",
                           "float input '" + name + "' min must be finite");
                spec.minimum = minimum.get<double>();
            } else {
                spec.minimum = static_cast<double>(readInt32(minimum, name, "min",
                                                            "live_input.range@1"));
            }
        }
        if (has_max) {
            const auto &maximum = entry["max"];
            if (type == LiveInputType::floating) {
                if (!maximum.is_number() || !std::isfinite(maximum.get<double>()))
                    refuse("live_input.range@1",
                           "float input '" + name + "' max must be finite");
                spec.maximum = maximum.get<double>();
            } else {
                spec.maximum = static_cast<double>(readInt32(maximum, name, "max",
                                                            "live_input.range@1"));
            }
        }
        if (spec.minimum && spec.maximum && *spec.minimum > *spec.maximum)
            refuse("live_input.range@1",
                   "input '" + name + "' min is greater than max");
        if ((spec.minimum && spec.default_value < *spec.minimum) ||
            (spec.maximum && spec.default_value > *spec.maximum))
            refuse("live_input.default_range@1",
                   "input '" + name + "' default is outside its declared range");
        specs.push_back(std::move(spec));
    }
    return specs;
}

double clampTo(const LiveInputSpec &spec, double value, bool &clamped) {
    if (spec.minimum && value < *spec.minimum) {
        clamped = true;
        value = *spec.minimum;
    }
    if (spec.maximum && value > *spec.maximum) {
        clamped = true;
        value = *spec.maximum;
    }
    if (spec.type == LiveInputType::integer) value = std::trunc(value);
    return value;
}

LiveInputSnapshot snapshotOf(const LiveInputSpec &spec, double value, bool from_rpc,
                             std::uint64_t changed_frame, bool clamped) {
    LiveInputSnapshot snapshot;
    snapshot.spec = spec;
    snapshot.value = value;
    snapshot.from_rpc = from_rpc;
    snapshot.changed_frame = changed_frame;
    snapshot.clamped = clamped;
    return snapshot;
}

} // namespace

std::string_view liveInputTypeName(LiveInputType type) noexcept {
    switch (type) {
    case LiveInputType::floating: return "float";
    case LiveInputType::integer: return "int";
    case LiveInputType::boolean: return "bool";
    }
    return "float";
}

LiveInputs::LiveInputs() {
    if (const auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>()) {
        if (const auto text = config->liveInputsJson()) {
            installDeclaration(parseDeclaration(*text));
            return;
        }
    }
    installDeclaration({});
}

void LiveInputs::installDeclaration(std::vector<LiveInputSpec> specs) {
    std::scoped_lock lock{mutex_};
    specs_ = std::move(specs);
    indices_.clear();
    runtime_.clear();
    pending_.clear();
    dropped_during_replay_ = 0;
    for (std::size_t index = 0; index < specs_.size(); ++index) {
        indices_.emplace(specs_[index].name, index);
        Runtime runtime;
        runtime.spec = specs_[index];
        runtime.value = specs_[index].default_value;
        runtime_.push_back(std::move(runtime));
    }
}

void LiveInputs::reloadDeclaration() {
    std::vector<LiveInputSpec> specs;
    if (const auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>()) {
        if (const auto text = config->liveInputsJson()) specs = parseDeclaration(*text);
    }
    installDeclaration(std::move(specs));
}

void LiveInputs::setDeclarationJson(std::string_view document) {
    installDeclaration(parseDeclaration(document));
}

bool LiveInputs::isDeclared(std::string_view name) const {
    std::scoped_lock lock{mutex_};
    return indices_.contains(name);
}

std::optional<LiveInputSpec> LiveInputs::declaration(std::string_view name) const {
    std::scoped_lock lock{mutex_};
    const auto found = indices_.find(name);
    if (found == indices_.end()) return std::nullopt;
    return specs_[found->second];
}

std::vector<LiveInputSpec> LiveInputs::declarations() const {
    std::scoped_lock lock{mutex_};
    return specs_;
}

std::size_t LiveInputs::stageValues(const nlohmann::json &values) {
    std::scoped_lock lock{mutex_};
    if (replaying_) {
        ++dropped_during_replay_;
        refuse("live_input.replay@1",
               "set_live_input cannot be combined with start_input_replay");
    }
    if (!values.is_object())
        refuse("live_input.type@1", "values must be an object of input names");
    struct Staged {
        std::size_t index = 0;
        double value = 0.0;
        bool clamped = false;
    };
    std::vector<Staged> staged;
    staged.reserve(values.size());
    for (const auto &[name, value] : values.items()) {
        const auto found = indices_.find(name);
        if (found == indices_.end())
            refuse("live_input.unknown@1", "no live input named '" + name + "'");
        const auto &spec = specs_[found->second];
        double staged_value = 0.0;
        bool clamped = false;
        switch (spec.type) {
        case LiveInputType::floating:
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                refuse("live_input.type@1",
                       "live input '" + name + "' is float and needs a finite number");
            staged_value = value.get<double>();
            break;
        case LiveInputType::integer: {
            if (!value.is_number_integer())
                refuse("live_input.type@1",
                       "live input '" + name + "' is int and needs a JSON integer");
            staged_value = static_cast<double>(
                readInt32(value, name, "value", "live_input.type@1"));
            break;
        }
        case LiveInputType::boolean:
            if (!value.is_boolean())
                refuse("live_input.type@1",
                       "live input '" + name + "' is bool and needs a JSON boolean");
            staged_value = value.get<bool>() ? 1.0 : 0.0;
            break;
        }
        staged_value = clampTo(spec, staged_value, clamped);
        staged.push_back({found->second, staged_value, clamped});
    }
    // Atomic commit: every name and value was valid before this point.
    for (const auto &entry : staged) {
        pending_[specs_[entry.index].name] = Pending{entry.value, entry.clamped};
    }
    return staged.size();
}

void LiveInputs::freeze(bool replaying, std::uint64_t frame) {
    std::scoped_lock lock{mutex_};
    if (replaying != replaying_) {
        replaying_ = replaying;
        dropped_during_replay_ = 0;
        pending_.clear();
        resetFrozenLocked();
    }
    if (replaying_) {
        dropped_during_replay_ += pending_.size();
        pending_.clear();
        return;
    }
    for (const auto &[name, pending] : pending_) {
        const auto found = indices_.find(name);
        if (found == indices_.end()) continue;
        auto &runtime = runtime_[found->second];
        if (runtime.value != pending.value) {
            runtime.value = pending.value;
            runtime.changed_frame = frame;
        }
        runtime.from_rpc = true;
        runtime.clamped = pending.clamped;
    }
    pending_.clear();
}

void LiveInputs::resetFrozenLocked() {
    for (auto &runtime : runtime_) {
        runtime.value = runtime.spec.default_value;
        runtime.from_rpc = false;
        runtime.changed_frame = 0;
        runtime.clamped = false;
    }
}

ValueGraph::Inputs LiveInputs::frozenInputs() const {
    std::scoped_lock lock{mutex_};
    ValueGraph::Inputs inputs;
    for (const auto &runtime : runtime_)
        inputs.emplace("live:" + runtime.spec.name, runtime.value);
    return inputs;
}

std::vector<LiveInputSnapshot> LiveInputs::snapshot() const {
    std::scoped_lock lock{mutex_};
    std::vector<LiveInputSnapshot> snapshots;
    snapshots.reserve(runtime_.size());
    for (const auto &runtime : runtime_)
        snapshots.push_back(snapshotOf(runtime.spec, runtime.value, runtime.from_rpc,
                                       runtime.changed_frame, runtime.clamped));
    return snapshots;
}

bool LiveInputs::isReplaying() const {
    std::scoped_lock lock{mutex_};
    return replaying_;
}

std::uint64_t LiveInputs::droppedDuringReplay() const {
    std::scoped_lock lock{mutex_};
    return dropped_during_replay_;
}

void LiveInputs::noteGraphReadsLive(std::string_view name) {
    std::scoped_lock lock{mutex_};
    if (graph_reads_live_) return;
    graph_reads_live_ = true;
    first_live_read_ = name;
}

bool LiveInputs::graphReadsLive() const {
    std::scoped_lock lock{mutex_};
    return graph_reads_live_;
}

std::string LiveInputs::firstLiveRead() const {
    std::scoped_lock lock{mutex_};
    return first_live_read_;
}

} // namespace Pelican
