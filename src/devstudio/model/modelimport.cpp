#include "modelimport.hpp"

#include "graphcanvasmodel.hpp"
#include "inspectormodel.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <tuple>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

std::string lowerExtension(const std::filesystem::path &file) {
    auto extension = utf8(file.extension());
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

// `file` relative to `root` when it is inside it, else empty.
std::filesystem::path insideProject(const std::filesystem::path &root, const std::filesystem::path &file) {
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error) return {};
    const auto canonical_file = std::filesystem::weakly_canonical(file, error);
    if (error) return {};
    auto relative = canonical_file.lexically_relative(canonical_root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return {};
    return relative;
}

std::string text(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// applied / restart_required of a register_model or unregister_model answer.
std::pair<bool, std::string> applied(const Json &result) {
    const bool live = text(result, "applied") != "restart_required";
    std::string reason;
    if (const auto restart = result.find("restart_required"); restart != result.end() && restart->is_object())
        reason = text(*restart, "reason");
    if (!live && reason.empty()) reason = "the engine reads it when it starts again";
    return {live, reason};
}

std::string sentenceOf(const std::string &code, const std::string &message, const Json &fields) {
    auto sentence = modelRefusalDescription(
        Json{{"code", code}, {"message", message}, {"error_fields", fields.is_object() ? fields : Json::object()}});
    if (!sentence.empty()) return sentence;
    return code.empty() || message.starts_with(code) ? message : message + " (" + code + ")";
}

} // namespace

ProjectModelFile placeProjectModelFile(const std::filesystem::path &project_root, const std::filesystem::path &file) {
    ProjectModelFile placed;
    if (project_root.empty()) {
        placed.error = "Open a project to add a model to.";
        return placed;
    }
    const auto extension = lowerExtension(file);
    if (extension != ".glb" && extension != ".gltf" && extension != ".vrm") {
        placed.error = utf8(file.filename()) + " is not a .glb, .gltf or .vrm file.";
        return placed;
    }
    if (const auto relative = insideProject(project_root, file); !relative.empty()) {
        const auto generic = relative.generic_u8string();
        placed.path.assign(generic.begin(), generic.end());
        return placed;
    }
    if (extension == ".gltf") {
        placed.error = "A .gltf reads the .bin and images beside it: put it in the project folder with "
                       "them first and choose it there (a model in one file is a .glb).";
        return placed;
    }
    const auto copied = copyIntoProjectAssets(project_root, "models", file);
    if (!copied.error.empty()) {
        placed.error = copied.error;
        return placed;
    }
    placed.path = copied.reference;
    placed.copy = copied.copy;
    return placed;
}

std::string projectRelativeAssetPath(const std::filesystem::path &project_root, std::string_view path) {
    if (path.empty() || path.starts_with("engine://") || project_root.empty()) return {};
    if (path.starts_with("project://")) path.remove_prefix(10);
    const std::filesystem::path file{std::u8string{reinterpret_cast<const char8_t *>(path.data()), path.size()}};
    if (!file.is_absolute()) {
        const auto generic = file.lexically_normal().generic_u8string();
        if (generic.starts_with(u8"..")) return {};
        return {generic.begin(), generic.end()};
    }
    const auto relative = insideProject(project_root, file);
    if (relative.empty()) return {};
    const auto generic = relative.generic_u8string();
    return {generic.begin(), generic.end()};
}

std::string numberedModelName(std::string_view stem, unsigned attempt) {
    if (attempt == 0) return {};
    std::string number = std::to_string(attempt);
    if (number.size() < 3) number.insert(0, 3 - number.size(), '0');
    return std::string{stem} + "_" + number;
}

ModelRegistrationSession::ModelRegistrationSession(Transport transport) : transport_{std::move(transport)} {}

bool ModelRegistrationSession::registerFile(const std::filesystem::path &project_root,
                                            const std::filesystem::path &file,
                                            std::function<void(const Registered &)> done, Refused refused) {
    const auto placed = placeProjectModelFile(project_root, file);
    if (!placed.error.empty()) {
        if (refused) refused(placed.error);
        return false;
    }
    Pending pending;
    pending.kind = Kind::Register;
    pending.path = placed.path;
    pending.stem = utf8(std::filesystem::path{std::u8string{
                                                  reinterpret_cast<const char8_t *>(placed.path.data()),
                                                  placed.path.size()}}
                            .stem());
    pending.copy = placed.copy;
    pending.registered = std::move(done);
    pending.refused = std::move(refused);
    return sendRegister(std::move(pending));
}

bool ModelRegistrationSession::sendRegister(Pending pending) {
    Json params{{"path", pending.path}};
    if (const auto name = numberedModelName(pending.stem, pending.attempt); !name.empty()) params["name"] = name;
    std::string error;
    const auto id = transport_.request ? transport_.request("register_model", params, &error) : 0;
    if (id == 0) {
        refuse(pending, "Could not register " + pending.path + ": " +
                            (error.empty() ? std::string{"the engine is not running"} : error) + ".");
        return false;
    }
    pending_[id] = std::move(pending);
    return true;
}

void ModelRegistrationSession::refuse(Pending &pending, const std::string &sentence) {
    // Only the copy this made; a file that was in the project stays.
    if (!pending.copy.empty()) {
        std::error_code error;
        std::filesystem::remove(pending.copy, error);
    }
    if (pending.refused) pending.refused(sentence);
}

bool ModelRegistrationSession::askBindings(const std::string &model, std::function<void(bool bound)> answered) {
    std::string error;
    const auto id =
        transport_.request ? transport_.request("get_material_bindings", Json{{"model", model}}, &error) : 0;
    if (id == 0) return false;
    Pending pending;
    pending.kind = Kind::Bindings;
    pending.model = model;
    pending.bindings = std::move(answered);
    pending_[id] = std::move(pending);
    return true;
}

bool ModelRegistrationSession::unregister(const std::string &model, std::function<void(const Unregistered &)> done,
                                          Refused refused) {
    std::string error;
    const auto id = transport_.request ? transport_.request("unregister_model", Json{{"name", model}}, &error) : 0;
    if (id == 0) {
        if (refused)
            refused("Could not remove " + model + ": " +
                    (error.empty() ? std::string{"the engine is not running"} : error) + ".");
        return false;
    }
    Pending pending;
    pending.kind = Kind::Unregister;
    pending.model = model;
    pending.unregistered = std::move(done);
    pending.refused = std::move(refused);
    pending_[id] = std::move(pending);
    return true;
}

bool ModelRegistrationSession::owns(std::int64_t id) const { return pending_.contains(id); }

bool ModelRegistrationSession::receiveResult(std::int64_t id, const Json &result) {
    const auto found = pending_.find(id);
    if (found == pending_.end()) return false;
    auto pending = std::move(found->second);
    pending_.erase(found);
    switch (pending.kind) {
    case Kind::Register: {
        Registered registered;
        registered.name = text(result, "name");
        registered.path = text(result, "path");
        if (registered.path.empty()) registered.path = pending.path;
        std::tie(registered.live, registered.restart_reason) = applied(result);
        if (registered.name.empty()) {
            refuse(pending, "register_model answered without a name.");
            break;
        }
        if (pending.registered) pending.registered(registered);
        break;
    }
    case Kind::Bindings: {
        const auto bindings = result.is_object() ? result.find("material_bindings") : result.end();
        if (pending.bindings) pending.bindings(result.is_object() && bindings != result.end() && !bindings->is_null());
        break;
    }
    case Kind::Unregister: {
        Unregistered unregistered;
        unregistered.name = text(result, "name");
        if (unregistered.name.empty()) unregistered.name = pending.model;
        if (const auto removed = result.find("removed"); removed != result.end())
            unregistered.path = text(*removed, "path");
        std::tie(unregistered.live, unregistered.restart_reason) = applied(result);
        if (pending.unregistered) pending.unregistered(unregistered);
        break;
    }
    }
    return true;
}

bool ModelRegistrationSession::receiveRefusal(std::int64_t id, const std::string &code, const std::string &message,
                                              const Json &fields) {
    const auto found = pending_.find(id);
    if (found == pending_.end()) return false;
    auto pending = std::move(found->second);
    pending_.erase(found);
    switch (pending.kind) {
    case Kind::Register:
        if (code == "model.name_conflict@1") {
            if (pending.attempt >= MaximumNameAttempts) {
                refuse(pending, "No free model name for " + pending.stem + " (model.name_conflict@1).");
                break;
            }
            ++pending.attempt;
            sendRegister(std::move(pending));
            break;
        }
        if (code == "model.path_conflict@1" && !text(fields, "name").empty()) {
            // The file was registered already: the same model, chosen again.
            Registered registered{.name = text(fields, "name"), .path = pending.path, .already = true};
            if (pending.registered) pending.registered(registered);
            break;
        }
        refuse(pending, sentenceOf(code, message, fields));
        break;
    case Kind::Bindings:
        if (pending.bindings) pending.bindings(false);
        break;
    case Kind::Unregister:
        if (pending.refused) pending.refused(sentenceOf(code, message, fields));
        break;
    }
    return true;
}

bool ModelRegistrationSession::receiveFailure(std::int64_t id, const std::string &message) {
    const auto found = pending_.find(id);
    if (found == pending_.end()) return false;
    auto pending = std::move(found->second);
    pending_.erase(found);
    if (pending.kind == Kind::Bindings) {
        if (pending.bindings) pending.bindings(false);
        return true;
    }
    // Not known whether it was written: the copy stays.
    pending.copy.clear();
    refuse(pending, message);
    return true;
}

void ModelRegistrationSession::engineGone() { pending_.clear(); }

} // namespace PelicanStudio
