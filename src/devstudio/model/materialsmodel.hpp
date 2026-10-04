#pragma once

#include "assetsdockmodel.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// MT-1s. The material editor: the Materials dock (beside Assets) and the
// Material rows of the Inspector. Studio never writes a material file
// itself (D0): every read and write is one of the engine's MT-1 requests --
// list_materials, get_material, get_surface_schema, edit_material,
// create_material, get_material_bindings, bind_material -- and the surfaces
// a new material can take are the engine's list_surfaces (MT-1b). Studio
// keeps no list of surfaces of its own.
//
// Qt-free: the transport sends a request and answers its id, and every reply
// comes back through receiveResult / receiveRefusal / receiveFailure.

// --- list_materials

struct MaterialListMaterial {
    std::string name;
    std::string surface;
    std::vector<std::string> tags;
    std::string id;
    bool registered = false;
    bool operator==(const MaterialListMaterial &) const = default;
};
struct MaterialListDocument {
    std::string path;
    std::string digest;
    // "loaded", "missing" or "invalid" (then `reason` says why).
    std::string status;
    std::string reason;
    std::vector<MaterialListMaterial> materials;
    bool operator==(const MaterialListDocument &) const = default;
};
struct MaterialList {
    std::string asset_data_digest;
    std::vector<MaterialListDocument> documents;
    bool operator==(const MaterialList &) const = default;
};
[[nodiscard]] MaterialList parseMaterialList(const nlohmann::json &result);
// Every material name of the loaded documents, in the list's order.
[[nodiscard]] std::vector<std::string> materialNames(const MaterialList &list);
// Every tag a listed material carries, sorted, once each.
[[nodiscard]] std::vector<std::string> materialTagVocabulary(const MaterialList &list);
// The document that holds `name`, or nullptr.
[[nodiscard]] const MaterialListDocument *materialDocumentOf(const MaterialList &list,
                                                             std::string_view name);

// --- get_surface_schema

struct SurfaceParam {
    std::string name;
    // "float", "int", "vec2", "vec3", "vec4", "color".
    std::string type;
    nlohmann::json default_value;
    std::optional<double> min;
    std::optional<double> max;
    std::string hint;
    // "linear" or "srgb" (a color's), as the declaration says.
    std::string encoding;
};
struct SurfaceTexture {
    std::string name;
    std::string default_reference;
    std::string color_space;
    std::string role;
    // "2d", "cube", "2d_array", "3d".
    std::string dimension;
};
struct SurfaceSchema {
    std::string surface;
    std::vector<SurfaceParam> params;
    std::vector<SurfaceTexture> textures;
};
[[nodiscard]] SurfaceSchema parseSurfaceSchema(const nlohmann::json &result);

// How a param is edited (decision 2): a float with both a min and a max is a
// slider and a spin; a float without them a spin; an int a spin (a check
// when its hint is "bool" or "toggle"); a color -- or a vec3 / vec4 whose
// hint is "color" -- a swatch; any other vector a spin per component.
enum class MaterialFieldKind : std::uint8_t { Slider, Number, Integer, Toggle, Color, Vector, Other };
[[nodiscard]] MaterialFieldKind materialFieldKind(const SurfaceParam &param);

// The value the material gives `param` (its values[param]), or the
// declaration's default when it gives none.
[[nodiscard]] nlohmann::json materialParamValue(const nlohmann::json &material, const SurfaceParam &param);
// The material sets `param` itself, to something other than its default:
// the row's name is bold.
[[nodiscard]] bool materialParamOverridden(const nlohmann::json &material, const SurfaceParam &param);
// The texture the material names for `texture`, or empty.
[[nodiscard]] std::string materialTextureValue(const nlohmann::json &material, std::string_view texture);

// A swatch shows a color as the eye sees it (sRGB); a "linear" color is
// stored linear and converted both ways, an "srgb" one is stored as shown.
[[nodiscard]] double srgbFromLinear(double linear);
[[nodiscard]] double linearFromSrgb(double srgb);
[[nodiscard]] std::array<double, 4> materialColorForDisplay(const nlohmann::json &value,
                                                            const SurfaceParam &param);
// The stored value for a shown color, with as many components as `param`'s
// default has (3 or 4), rounded to six places.
[[nodiscard]] nlohmann::json materialColorFromDisplay(const std::array<double, 4> &shown,
                                                      const SurfaceParam &param);

// --- list_surfaces (engine MT-1b)

struct SurfaceChoice {
    std::string reference;
    std::string source;  // "engine" or "project"
    std::string name;
    std::uint64_t params = 0;
    std::uint64_t textures = 0;
    nlohmann::json routing;  // null when the surface implies none
    std::string error;       // a surface that cannot be read: listed, not chosen
};
[[nodiscard]] std::vector<SurfaceChoice> parseSurfaceList(const nlohmann::json &result);
// How a surface reads in a combo: its file without ".surface" and the
// folders above it under surfaces/ ("openpbr/opaque_single", a project's
// "shaders/toon"); a variant the engine names reads as its family and its
// name ("openpbr / opaque_single_sided").
[[nodiscard]] std::string surfaceChoiceLabel(const SurfaceChoice &surface);
// The one a new material starts on: the engine's OpenPBR opaque one-sided
// surface when listed, else the first one that can be read; nullopt for none.
[[nodiscard]] std::optional<std::size_t> defaultSurfaceChoice(const std::vector<SurfaceChoice> &surfaces);

// --- a new material's name and file (decision 4)

// "material", else "material_2", "material_3", ...: free among the listed
// names and with no file at materials/<name>.material.json under
// `project_root` (an empty root checks no file).
[[nodiscard]] std::string suggestMaterialName(const MaterialList &list,
                                              const std::filesystem::path &project_root,
                                              std::string_view base = "material");
// "materials/<name>.material.json".
[[nodiscard]] std::string materialDocumentPath(std::string_view name);
// Letters, digits and '_' (what create_material takes).
[[nodiscard]] bool validMaterialName(std::string_view name);

// --- texture slots (decision 2)

// The dimension of an image file as a texture slot reads it: a KTX2's from
// its header ("2d", "cube", "2d_array", "3d"), any other image "2d"; empty
// for a file that is no image.
[[nodiscard]] std::string textureFileDimension(const std::filesystem::path &file);
// The project's images of one dimension, as `project://<logical path>`,
// sorted.
[[nodiscard]] std::vector<std::string> textureChoices(const std::vector<AssetEntry> &entries,
                                                      std::string_view dimension);

// --- assignment (decision 5)

struct MaterialPrimitive {
    std::uint64_t mesh = 0;
    std::uint64_t primitive = 0;
    // The model's own material for the primitive (its glTF material, or
    // pelican.primitive.default for an engine primitive).
    std::string slot;
    std::string material;
    bool operator==(const MaterialPrimitive &) const = default;
};
[[nodiscard]] std::vector<MaterialPrimitive> parseMaterialBindings(const nlohmann::json &result);
// MT-2s, decision 4 (engine MT-2): get_material_bindings' `objects` (the scene
// objects that draw the model, every scene, prefab-made ones too) and
// `object_overrides` (those of them with materials of their own); nullopt
// from an engine that does not count them.
struct ModelObjectCounts {
    std::optional<std::size_t> objects;
    std::optional<std::size_t> overrides;
};
[[nodiscard]] ModelObjectCounts parseModelObjectCounts(const nlohmann::json &result);
// How a model reads in a line: a built-in primitive by its name ("Cube"),
// any other model by its reference.
[[nodiscard]] std::string modelShortName(std::string_view model);
// "Applies to every Cube in this project (2 objects, 1 with their own
// material)." -- what is not known left out.
[[nodiscard]] std::string modelScopeLine(std::string_view model, const ModelObjectCounts &counts);
// "Applies to Cube_001 only." (MT-2s, decision 1: This object).
[[nodiscard]] std::string objectScopeLine(std::string_view object);

// --- an object's own materials (MT-2s, engine MT-2's get_object_materials)

struct ObjectMaterialRow {
    std::uint64_t row_id = 0;
    std::string material;
    // The primitive the row is for; nullopt: every primitive.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> primitive;
    // false: no material of that name is loaded, and the model's draws.
    bool resolved = true;
    bool operator==(const ObjectMaterialRow &) const = default;
};
struct ObjectMaterialPrimitive {
    std::uint64_t mesh = 0;
    std::uint64_t primitive = 0;
    std::string slot;
    // What the primitive carries: the object's row when one applies, else
    // the model's.
    std::string material;
    // "object" or "model".
    std::string source;
    std::string model_material;
    // What is drawn now (empty when the object has no live instance).
    std::string drawn;
    bool operator==(const ObjectMaterialPrimitive &) const = default;
};
struct ObjectMaterials {
    std::uint64_t object = 0;
    std::string scene_id;
    std::string model;
    std::vector<ObjectMaterialRow> rows;
    std::vector<ObjectMaterialPrimitive> primitives;
    bool operator==(const ObjectMaterials &) const = default;
};
[[nodiscard]] ObjectMaterials parseObjectMaterials(const nlohmann::json &result);
// Decision 2: what a primitive reads as -- the drawn material (the one it
// carries when nothing is drawn), "paint (this object)" when the object's own
// row gives it, "(model default: pelican.primitive.default)" when the model's.
[[nodiscard]] std::string objectMaterialLabel(const ObjectMaterialPrimitive &primitive);
// Decision 7: "Material <name> is missing; drawing the model default" for the
// first row whose material is not loaded; empty when every row resolves.
[[nodiscard]] std::string objectMaterialMissingLine(const ObjectMaterials &materials);

// --- the Materials dock

class MaterialsDockSession {
  public:
    using Json = nlohmann::json;

    // list_materials is read this often while the dock is shown; a reading
    // whose digests are the ones held changes nothing.
    static constexpr int PollMs = 1000;

    struct Transport {
        std::function<std::int64_t(const std::string &method, const Json &params, std::string *error)>
            request;
    };

    struct Selection {
        std::string path;
        std::string name;
        bool operator==(const Selection &) const = default;
    };

    // One entry of the dock's Undo: an edit_material, taken back by sending
    // its `previous` against the document's digest then; or a
    // bind_material, taken back with its previous.bindings.
    struct UndoEntry {
        enum class Kind : std::uint8_t { Edit, Bind };
        Kind kind = Kind::Edit;
        std::string description;
        std::string path;
        std::string name;
        std::string model;
        Json previous;
    };

    enum class SurfacesState : std::uint8_t { Unknown, Reading, Listed, Unavailable };

    explicit MaterialsDockSession(Transport transport);

    // Anything shown may read differently.
    std::function<void()> changed;
    // The list's digests moved (a material made, renamed, edited outside).
    std::function<void()> listChanged;
    // An edit was written but the engine applies it at its next start.
    std::function<void(const std::string &reason)> restartRequested;
    // A bind_material of this dock committed (the Inspector reads its rows
    // again).
    std::function<void(const std::string &model)> bound;

    void open(std::filesystem::path project_root);
    void engineReady();
    void engineGone();
    // The dock's clock: list_materials again (nothing while one is out).
    void poll();

    // Decision 1: the material the editor shows (get_material, then its
    // surface's get_surface_schema).
    void select(const Selection &selection);
    void clearSelection();

    // Decisions 2 and 3: one edit_material each, against the digest held.
    // `value` null takes the material's own value away (the default shows).
    bool setValue(const std::string &param, const Json &value);
    bool setTexture(const std::string &texture, const Json &reference);
    bool setTags(const std::vector<std::string> &tags);
    bool setRouting(const std::string &alpha_mode, bool double_sided);
    bool setSurface(const std::string &surface);
    // Decision 4: create_material on `surface`; the new one is selected once
    // it is listed. Not undoable (there is no delete_material).
    bool create(const SurfaceChoice &surface, const std::string &name);
    // Decision 5 (b): bind_material of the selected material to every
    // primitive of `model`; `object` names it in Undo.
    bool assign(const std::string &model, const std::string &object);
    bool undo();
    // list_surfaces again (New material… opened).
    void readSurfaces();
    // A line of the dock's own.
    void say(std::string line);

    [[nodiscard]] bool owns(std::int64_t id) const noexcept;
    bool receiveResult(std::int64_t id, const Json &result);
    // A JSON-RPC error: its numeric code (-32601 for a method the engine does
    // not know), data.error_code and data.error_fields.
    bool receiveRefusal(std::int64_t id, int code, const std::string &error_code, const std::string &message,
                        const Json &error_fields);
    bool receiveFailure(std::int64_t id, const std::string &message);

    [[nodiscard]] const std::filesystem::path &projectRoot() const noexcept { return root_; }
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] bool listed() const noexcept { return listed_; }
    [[nodiscard]] const MaterialList &list() const noexcept { return list_; }
    [[nodiscard]] const std::optional<Selection> &selection() const noexcept { return selection_; }
    // The selected material as the document holds it (null until read).
    [[nodiscard]] const Json &material() const noexcept { return material_; }
    [[nodiscard]] const std::optional<SurfaceSchema> &schema() const noexcept { return schema_; }
    [[nodiscard]] const std::vector<SurfaceChoice> &surfaces() const noexcept { return surfaces_; }
    [[nodiscard]] SurfacesState surfacesState() const noexcept { return surfaces_state_; }
    [[nodiscard]] bool editPending() const noexcept { return write_ != 0 || !queue_.empty(); }
    // The selected material is being read (after an edit, or written
    // outside): material() is the one before it until the reply.
    [[nodiscard]] bool materialReading() const noexcept { return material_request_ != 0; }
    [[nodiscard]] const std::string &notice() const noexcept { return notice_; }
    [[nodiscard]] const std::string &noticeDetail() const noexcept { return notice_detail_; }
    // Decision 7: material.value_invalid@1's sentence, shown under its row.
    [[nodiscard]] const std::string &fieldErrorParam() const noexcept { return field_error_param_; }
    [[nodiscard]] const std::string &fieldError() const noexcept { return field_error_; }
    [[nodiscard]] const std::string &restartReason() const noexcept { return restart_reason_; }
    [[nodiscard]] const std::vector<UndoEntry> &undoEntries() const noexcept { return undo_; }
    // The digest a write of `path` goes against.
    [[nodiscard]] std::string digestOf(const std::string &path) const;

  private:
    struct Write {
        enum class Kind : std::uint8_t { Edit, Create, Bind, UndoEdit, UndoBind };
        Kind kind = Kind::Edit;
        std::string method;
        std::string path;
        std::string name;
        std::string model;
        std::string param;
        std::string description;
        Json params;
    };

    void notify();
    void setNotice(std::string line, std::string detail = {});
    std::int64_t send(const std::string &method, const Json &params);
    bool enqueue(Write write);
    void sendNext();
    void readList();
    void readSelected();
    void readSchema(const std::string &surface);
    bool editSelected(Json params, std::string description, std::string param = {});
    void listResult(const Json &result);
    void writeResult(const Write &write, const Json &result);
    void forgetRequests();

    Transport transport_;
    std::filesystem::path root_;
    bool ready_ = false;
    bool listed_ = false;
    MaterialList list_;
    std::map<std::string, std::string> digests_;
    std::optional<Selection> selection_;
    std::optional<Selection> select_when_listed_;
    Json material_;
    std::optional<SurfaceSchema> schema_;
    std::string schema_surface_;
    std::vector<SurfaceChoice> surfaces_;
    SurfacesState surfaces_state_ = SurfacesState::Unknown;
    std::string notice_;
    std::string notice_detail_;
    std::string field_error_param_;
    std::string field_error_;
    std::string restart_reason_;
    std::vector<UndoEntry> undo_;
    std::deque<Write> queue_;
    std::optional<Write> in_flight_;
    // The requests out.
    std::int64_t list_request_ = 0;
    std::int64_t material_request_ = 0;
    std::int64_t schema_request_ = 0;
    std::int64_t surfaces_request_ = 0;
    std::int64_t write_ = 0;
};

// --- the Inspector's Material rows

class MaterialAssignSession {
  public:
    using Json = nlohmann::json;

    struct Transport {
        std::function<std::int64_t(const std::string &method, const Json &params, std::string *error)>
            request;
        // MT-2s, decision 5: set_object_material is a scene edit, so it goes
        // through the Inspector's editor session -- its actor, its revision,
        // its Undo -- which adds actor_id and base_scene_revision. false (the
        // reason in *error) when that session could not take it.
        std::function<bool(const Json &params, const std::string &description, std::string *error)>
            edit_object;
    };

    // MT-2s, decision 1: what a choice applies to.
    enum class Scope : std::uint8_t { Object, Model };

    // The selected object, as the Inspector's snapshot holds it.
    struct Subject {
        std::string model;
        std::string name;
        std::uint64_t object = 0;
        std::string scene_id;
        // The scene revision the snapshot was read at: another one means the
        // object's rows may have moved (an edit, an Undo, another client).
        std::uint64_t revision = 0;
        bool operator==(const Subject &) const = default;
    };

    // A bind_material of the Material rows, taken back with its
    // previous.bindings. set_object_material is not here: it is the
    // scene's Undo (decision 5). `model` is the model the bind was sent to:
    // Undo sends the bindings back there, whatever is selected by then.
    struct UndoEntry {
        std::string description;
        std::string model;
        Json bindings = Json::array();
    };

    explicit MaterialAssignSession(Transport transport);

    std::function<void()> changed;
    std::function<void(const std::string &reason)> restartRequested;

    void engineReady();
    void engineGone();
    // The selected object (an empty model: none, or no simplemodelview).
    void setSubject(const Subject &subject);
    // The Inspector's editor session has an edit in flight (one of these
    // rows' set_object_material among them): the rows wait for it, then read
    // the object again.
    void setSceneBusy(bool busy);
    // The materials or the model's bindings changed elsewhere (the dock).
    void materialsChanged();
    void bindingsChanged();

    // Decision 1. This object (the default) or Every <model>.
    void setScope(Scope scope);
    [[nodiscard]] Scope scope() const noexcept { return scope_; }

    // Every primitive. This object: set_object_material of the object's
    // all-primitive row (decision 1). Every <model>: bind_material of the
    // model, an empty material being "Model default" (each primitive back to
    // its own slot) (MT-1s decision 5 (a)).
    bool choose(const std::string &material);
    // One primitive (an index of primitives() / objectMaterials().primitives),
    // as the scope says (decision 3). This object and an empty material: the
    // primitive draws the model's again.
    bool choosePrimitive(std::size_t index, const std::string &material);
    // Decision 2: every row of the object taken away (material: null).
    bool useModelDefault();
    [[nodiscard]] bool canUseModelDefault() const noexcept;
    bool undo();

    // Decision 7: the engine refused this session's set_object_material
    // (the Inspector's session handed its refusal here).
    void objectEditRefused(const std::string &error_code, const std::string &message, const Json &error_fields);

    [[nodiscard]] bool owns(std::int64_t id) const noexcept;
    bool receiveResult(std::int64_t id, const Json &result);
    bool receiveRefusal(std::int64_t id, int code, const std::string &error_code, const std::string &message,
                        const Json &error_fields);
    bool receiveFailure(std::int64_t id, const std::string &message);

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const std::string &model() const noexcept { return subject_.model; }
    [[nodiscard]] const Subject &subject() const noexcept { return subject_; }
    [[nodiscard]] const std::vector<std::string> &materials() const noexcept { return materials_; }
    [[nodiscard]] const std::vector<MaterialPrimitive> &primitives() const noexcept { return primitives_; }
    [[nodiscard]] bool bindingsRead() const noexcept { return bindings_read_; }
    [[nodiscard]] const ModelObjectCounts &counts() const noexcept { return counts_; }
    [[nodiscard]] const ObjectMaterials &objectMaterials() const noexcept { return object_materials_; }
    [[nodiscard]] bool objectRead() const noexcept { return object_read_; }
    // The rows can take a choice now (read, nothing of theirs in flight).
    [[nodiscard]] bool choosable() const noexcept;
    // Every <model>: one material all primitives carry, "" when each
    // carries its own slot (Model default), nullopt when they differ.
    [[nodiscard]] std::optional<std::string> sharedMaterial() const;
    // Every <model>: the material asked for while its bind is out, else
    // sharedMaterial().
    [[nodiscard]] std::optional<std::string> shownMaterial() const;
    // This object: the one label every primitive reads as
    // (objectMaterialLabel), nullopt when they differ; the material asked for
    // while its edit is out.
    [[nodiscard]] std::optional<std::string> shownObjectLabel() const;
    [[nodiscard]] bool bindPending() const noexcept { return bind_request_ != 0; }
    [[nodiscard]] bool objectEditPending() const noexcept { return object_edit_pending_; }
    // The material whose set_object_material is out (nullopt: none, or Use
    // model default).
    [[nodiscard]] std::optional<std::string> objectRequested() const {
        return object_edit_pending_ ? object_requested_ : std::nullopt;
    }
    [[nodiscard]] const std::string &notice() const noexcept { return notice_; }
    [[nodiscard]] const std::string &noticeDetail() const noexcept { return notice_detail_; }
    [[nodiscard]] const std::vector<UndoEntry> &undoEntries() const noexcept { return undo_; }

  private:
    void notify();
    std::int64_t send(const std::string &method, const Json &params);
    void readAll();
    void readBindings();
    void readMaterials();
    void readObject();
    bool bind(Json params, std::string description, bool undoing,
              std::optional<std::string> requested = std::nullopt);
    bool editObject(Json params, std::string description, std::optional<std::string> requested);
    [[nodiscard]] const std::string &label() const noexcept;

    Transport transport_;
    bool ready_ = false;
    Subject subject_;
    Scope scope_ = Scope::Object;
    bool scene_busy_ = false;
    std::vector<std::string> materials_;
    std::vector<MaterialPrimitive> primitives_;
    bool bindings_read_ = false;
    ModelObjectCounts counts_;
    ObjectMaterials object_materials_;
    bool object_read_ = false;
    bool object_edit_pending_ = false;
    std::optional<std::string> object_requested_;
    std::string notice_;
    std::string notice_detail_;
    std::vector<UndoEntry> undo_;
    std::string bind_description_;
    // The model the bind out was sent to.
    std::string bind_model_;
    std::optional<std::string> requested_;
    bool bind_undoing_ = false;
    std::int64_t bindings_request_ = 0;
    std::int64_t materials_request_ = 0;
    std::int64_t object_request_ = 0;
    std::int64_t bind_request_ = 0;
};

} // namespace PelicanStudio
