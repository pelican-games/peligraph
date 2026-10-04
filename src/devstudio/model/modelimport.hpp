#pragma once

// MI-1s (engine MI-1): an outside model brought into a project from the GUI.
// Add Object > Model from file... and the Inspector's Import... beside the
// model field place the chosen file in the project (as it is when it is in
// the project already, else a copy under assets/models) and register it with
// register_model; the Assets dock's Remove from project... takes a
// registration back with unregister_model. This is the Qt-free part: where
// the file goes, what is sent, and how each answer is read.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace PelicanStudio {

// Decision 1: the files a model is chosen from.
inline constexpr std::string_view ModelFileFilter = "Models (*.glb *.gltf *.vrm *.GLB *.GLTF *.VRM)";

// Where a chosen file is registered from. A file inside the project is
// registered where it is (`copy` empty); one outside is copied into
// assets/models under a free name (`<stem>_2`, ...; copyIntoProjectAssets).
// An outside .gltf names its .bin and images beside it, so it is not copied
// alone: `error` says to put it in the project with them first (or to use a
// .glb). `path` is project-relative with forward slashes.
struct ProjectModelFile {
    std::string path;
    std::filesystem::path copy;
    std::string error;
};
[[nodiscard]] ProjectModelFile placeProjectModelFile(const std::filesystem::path &project_root,
                                                     const std::filesystem::path &file);

// Decision 4: a list_assets path as the project names it
// ("assets/models/box.glb"). Since engine EB-1 list_assets' `path` is the
// spelling asset_data holds -- project-relative, or project:// -- and the
// file it resolved is `resolved_path` (absolute); an absolute path is still
// read. Empty for an engine:// one and one outside the project.
[[nodiscard]] std::string projectRelativeAssetPath(const std::filesystem::path &project_root, std::string_view path);

// GL-5e, decision 9: the registered models of a list_assets result, by the
// file the project names them by -> their id. Only `kind: "model"` rows: the
// material rows EB-1 added (one per material, `id` its name) are not models.
[[nodiscard]] std::map<std::string, std::string> registeredModelFiles(const std::filesystem::path &project_root,
                                                                      const nlohmann::json &list_assets_result);

// The name register_model is sent with on try `attempt`: 0 sends none (the
// engine takes the file's stem), then "<stem>_001", "<stem>_002"... as
// uniqueObjectName numbers.
[[nodiscard]] std::string numberedModelName(std::string_view stem, unsigned attempt);

class ModelRegistrationSession {
  public:
    struct Transport {
        // Sends one editor request; the id its reply comes back under, or 0
        // with `error` saying why.
        std::function<std::int64_t(const std::string &method, const nlohmann::json &params, std::string *error)>
            request;
    };

    struct Registered {
        std::string name;
        std::string path;
        // applied: "live"; else restart_required's reason.
        bool live = true;
        std::string restart_reason;
        // model.path_conflict@1: the file was registered already, as `name`.
        bool already = false;
    };
    struct Unregistered {
        std::string name;
        std::string path;
        bool live = true;
        std::string restart_reason;
    };
    using Refused = std::function<void(const std::string &sentence)>;

    // The most names tried after the stem before giving up.
    static constexpr unsigned MaximumNameAttempts = 999;

    explicit ModelRegistrationSession(Transport transport);

    // Decision 1: places `file` and sends register_model {path} (no name).
    // model.name_conflict@1 sends it again as <stem>_001, _002...;
    // model.path_conflict@1 is the registration that is there already;
    // any other refusal removes the copy this made (only that) and says why.
    // False, with `refused` told, when nothing could be sent.
    bool registerFile(const std::filesystem::path &project_root, const std::filesystem::path &file,
                      std::function<void(const Registered &)> done, Refused refused);

    // Decision 4: whether `model` has material bindings (get_material_bindings'
    // material_bindings is not null), asked before its removal is confirmed.
    // A refusal answers false.
    bool askBindings(const std::string &model, std::function<void(bool bound)> answered);
    // unregister_model {name}.
    bool unregister(const std::string &model, std::function<void(const Unregistered &)> done, Refused refused);

    [[nodiscard]] bool owns(std::int64_t id) const;
    [[nodiscard]] bool busy() const noexcept { return !pending_.empty(); }
    bool receiveResult(std::int64_t id, const nlohmann::json &result);
    bool receiveRefusal(std::int64_t id, const std::string &code, const std::string &message,
                        const nlohmann::json &fields);
    bool receiveFailure(std::int64_t id, const std::string &message);
    // The engine went: what was asked is not answered. A copy stays (whether
    // it was registered is not known).
    void engineGone();

  private:
    enum class Kind { Register, Bindings, Unregister };
    struct Pending {
        Kind kind = Kind::Register;
        std::string path;
        std::string stem;
        unsigned attempt = 0;
        std::filesystem::path copy;
        std::string model;
        std::function<void(const Registered &)> registered;
        std::function<void(bool)> bindings;
        std::function<void(const Unregistered &)> unregistered;
        Refused refused;
    };

    bool sendRegister(Pending pending);
    void refuse(Pending &pending, const std::string &sentence);

    Transport transport_;
    std::map<std::int64_t, Pending> pending_;
};

} // namespace PelicanStudio
