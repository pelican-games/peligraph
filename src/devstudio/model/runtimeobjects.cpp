#include "runtimeobjects.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

std::optional<std::uint32_t> uint32Field(const Json &object, const char *key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) return std::nullopt;
    std::uint64_t value = 0;
    if (found->is_number_unsigned()) {
        value = found->get<std::uint64_t>();
    } else {
        const auto signed_value = found->get<std::int64_t>();
        if (signed_value < 0) return std::nullopt;
        value = static_cast<std::uint64_t>(signed_value);
    }
    if (value > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

template <std::size_t N>
std::optional<std::array<double, N>> numbers(const Json &object, const char *key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array() || found->size() != N) return std::nullopt;
    std::array<double, N> values{};
    for (std::size_t index = 0; index < N; ++index) {
        const auto &value = (*found)[index];
        if (!value.is_number()) return std::nullopt;
        values[index] = value.get<double>();
    }
    return values;
}

std::optional<RuntimeObjectTrs> trs(const Json &transform, const char *key) {
    const auto found = transform.find(key);
    if (found == transform.end() || !found->is_object()) return std::nullopt;
    return RuntimeObjectTrs{
        .pos = numbers<3>(*found, "pos"),
        .rotation = numbers<4>(*found, "rotation"),
        .scale = numbers<3>(*found, "scale"),
    };
}

} // namespace

std::optional<std::vector<RuntimeObject>> parseRuntimeObjects(const Json &result) {
    if (!result.is_object()) return std::nullopt;
    const auto listed = result.find("objects");
    if (listed == result.end() || !listed->is_array()) return std::nullopt;
    std::vector<RuntimeObject> objects;
    objects.reserve(listed->size());
    for (const auto &entry : *listed) {
        if (!entry.is_object()) continue;
        const auto name = entry.find("name");
        const auto entity = entry.find("entity");
        if (name == entry.end() || !name->is_string() || entity == entry.end() ||
            !entity->is_object())
            continue;
        const auto index = uint32Field(*entity, "index");
        const auto generation = uint32Field(*entity, "generation");
        if (!index || !generation) continue;
        RuntimeObject object{
            .name = name->get<std::string>(),
            .entity = {.index = *index, .generation = *generation},
        };
        if (const auto spawned_by = entry.find("spawned_by");
            spawned_by != entry.end() && spawned_by->is_object()) {
            const auto prefab = spawned_by->find("prefab");
            if (prefab != spawned_by->end() && prefab->is_string())
                object.prefab = prefab->get<std::string>();
        }
        if (const auto transform = entry.find("transform");
            transform != entry.end() && transform->is_object()) {
            object.local = trs(*transform, "local_trs");
            object.world = trs(*transform, "world_trs");
        }
        objects.push_back(std::move(object));
    }
    return objects;
}

void RuntimeObjectsModel::connect() {
    if (connected_) return;
    connected_ = true;
    changed();
    // The readiness probe's get_status is read before the engine is called
    // ready: a game already running is listed at once.
    if (running()) read();
}

void RuntimeObjectsModel::disconnect() {
    connected_ = false;
    supported_ = true;
    state_.reset();
    game_frame_.reset();
    scene_id_.clear();
    objects_.clear();
    selected_.reset();
    outgoing_.clear();
    in_flight_ = 0;
    read_again_ = false;
    changed();
}

bool RuntimeObjectsModel::observeStatus(const Json &status) {
    if (!status.is_object()) return false;
    if (const auto scene = status.find("scene"); scene != status.end() && scene->is_string())
        scene_id_ = scene->get<std::string>();
    // The play controls' own reading of the play state (PM-3b).
    const auto play = parsePlayStatus(status);
    if (!play) return false;
    const std::optional<std::uint64_t> frame = play->game_frame;

    const bool phase_moved = play->state != state_;
    // Paused, the list changes only with a step, and a step moves the game
    // frame. Playing, the clock reads it; a reading per get_status would be
    // as many reads as there are panels asking.
    const bool stepped = play->state == PlayState::paused && frame != game_frame_;
    state_ = play->state;
    game_frame_ = frame;
    if (state_ == PlayState::edit) {
        // Nothing is spawned in the edit state: the list is empty, and an
        // answer still on its way describes the game that was stopped.
        read_again_ = false;
        replace({});
        if (phase_moved) changed();
        return true;
    }
    if (phase_moved || stepped) read();
    if (phase_moved) changed();
    return true;
}

bool RuntimeObjectsModel::tick() {
    if (!tickWanted() || in_flight_ != 0) return false;
    read();
    return true;
}

bool RuntimeObjectsModel::tickWanted() const noexcept {
    return connected_ && supported_ && state_ == PlayState::playing;
}

std::vector<RuntimeObjectsRequest> RuntimeObjectsModel::takeRequests() {
    return std::exchange(outgoing_, {});
}

bool RuntimeObjectsModel::owns(std::uint64_t request_id) const noexcept {
    return request_id != 0 && request_id == in_flight_;
}

void RuntimeObjectsModel::receiveResult(std::uint64_t request_id, const Json &result) {
    if (!owns(request_id)) return;
    in_flight_ = 0;
    if (running()) {
        if (auto objects = parseRuntimeObjects(result)) replace(std::move(*objects));
    }
    if (std::exchange(read_again_, false) && running()) read();
}

void RuntimeObjectsModel::receiveFailure(std::uint64_t request_id, bool unknown_method) {
    if (!owns(request_id)) return;
    in_flight_ = 0;
    read_again_ = false;
    if (unknown_method) {
        // An engine from before RO-1: it has nothing to list, now or later.
        supported_ = false;
        replace({});
        changed();
    }
}

const RuntimeObject *RuntimeObjectsModel::find(const RuntimeObjectEntity &entity) const noexcept {
    const auto found = std::ranges::find_if(
        objects_, [&](const RuntimeObject &object) { return object.entity == entity; });
    return found == objects_.end() ? nullptr : &*found;
}

bool RuntimeObjectsModel::select(const RuntimeObjectEntity &entity) {
    if (find(entity) == nullptr) return false;
    if (selected_ != entity) {
        selected_ = entity;
        changed();
    }
    return true;
}

void RuntimeObjectsModel::clearSelection() {
    if (!selected_) return;
    selected_.reset();
    changed();
}

const RuntimeObject *RuntimeObjectsModel::selectedObject() const noexcept {
    return selected_ ? find(*selected_) : nullptr;
}

void RuntimeObjectsModel::read() {
    if (!connected_ || !supported_ || !running()) return;
    if (in_flight_ != 0) {
        read_again_ = true;
        return;
    }
    in_flight_ = next_request_++;
    outgoing_.push_back(
        {.request_id = in_flight_, .method = "get_runtime_objects", .params = Json::object()});
}

void RuntimeObjectsModel::replace(std::vector<RuntimeObject> objects) {
    if (objects == objects_) return;
    objects_ = std::move(objects);
    if (selected_ && find(*selected_) == nullptr) selected_.reset();
    changed();
}

} // namespace PelicanStudio
