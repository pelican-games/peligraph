#include "materialsmodel.hpp"

#include "inspectormodel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

std::string text(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::uint64_t counter(const Json &object, const char *key) {
    if (!object.is_object()) return 0;
    const auto found = object.find(key);
    if (found == object.end()) return 0;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (found->is_number_integer()) {
        const auto value = found->get<std::int64_t>();
        return value < 0 ? 0 : static_cast<std::uint64_t>(value);
    }
    return 0;
}

std::optional<double> number(const Json &object, const char *key) {
    if (!object.is_object()) return std::nullopt;
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) return std::nullopt;
    return found->get<double>();
}

std::vector<std::string> strings(const Json &value) {
    std::vector<std::string> list;
    if (!value.is_array()) return list;
    for (const auto &entry : value)
        if (entry.is_string()) list.push_back(entry.get<std::string>());
    return list;
}

std::string lower(std::string value) {
    for (char &c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

double rounded(double value) { return std::round(value * 1.0e6) / 1.0e6; }

// The error object the refusal tables read: {code, message, error_fields}.
Json refusalError(const std::string &error_code, const std::string &message, const Json &fields) {
    return Json{{"code", error_code},
                {"message", message},
                {"error_fields", fields.is_object() ? fields : Json::object()}};
}

std::string sentenceFor(const std::string &error_code, const std::string &message, const Json &fields) {
    auto sentence = materialRefusalDescription(refusalError(error_code, message, fields));
    return sentence.empty() ? message : sentence;
}

void flattenFiles(const std::vector<AssetEntry> &entries, std::vector<const AssetEntry *> &files) {
    for (const auto &entry : entries) {
        if (entry.directory) flattenFiles(entry.children, files);
        else files.push_back(&entry);
    }
}

void countUsers(const Json &value, std::string_view model, std::size_t &count) {
    if (value.is_array()) {
        for (const auto &entry : value) countUsers(entry, model, count);
        return;
    }
    if (!value.is_object()) return;
    if (const auto components = value.find("components");
        components != value.end() && components->is_array()) {
        for (const auto &component : *components)
            if (text(component, "name") == "simplemodelview" && text(component, "model") == model) {
                ++count;
                break;
            }
    }
    for (const auto &[key, member] : value.items())
        if (key != "components") countUsers(member, model, count);
}

} // namespace

// --- list_materials

MaterialList parseMaterialList(const Json &result) {
    MaterialList list;
    list.asset_data_digest = text(result, "asset_data_digest");
    if (!result.is_object()) return list;
    const auto documents = result.find("documents");
    if (documents == result.end() || !documents->is_array()) return list;
    for (const auto &document : *documents) {
        if (!document.is_object()) continue;
        MaterialListDocument entry;
        entry.path = text(document, "path");
        entry.digest = text(document, "material_digest");
        entry.status = text(document, "status");
        entry.reason = text(document, "reason");
        if (const auto materials = document.find("materials");
            materials != document.end() && materials->is_array())
            for (const auto &material : *materials) {
                if (!material.is_object()) continue;
                MaterialListMaterial row;
                row.name = text(material, "name");
                row.surface = text(material, "surface");
                row.tags = strings(material.value("tags", Json::array()));
                row.id = text(material, "id");
                row.registered = material.value("registered", false);
                if (!row.name.empty()) entry.materials.push_back(std::move(row));
            }
        list.documents.push_back(std::move(entry));
    }
    return list;
}

std::vector<std::string> materialNames(const MaterialList &list) {
    std::vector<std::string> names;
    for (const auto &document : list.documents)
        for (const auto &material : document.materials) names.push_back(material.name);
    return names;
}

std::vector<std::string> materialTagVocabulary(const MaterialList &list) {
    std::set<std::string> tags;
    for (const auto &document : list.documents)
        for (const auto &material : document.materials) tags.insert(material.tags.begin(), material.tags.end());
    return {tags.begin(), tags.end()};
}

const MaterialListDocument *materialDocumentOf(const MaterialList &list, std::string_view name) {
    for (const auto &document : list.documents)
        for (const auto &material : document.materials)
            if (material.name == name) return &document;
    return nullptr;
}

// --- get_surface_schema

SurfaceSchema parseSurfaceSchema(const Json &result) {
    SurfaceSchema schema;
    schema.surface = text(result, "surface");
    if (!result.is_object()) return schema;
    if (const auto params = result.find("params"); params != result.end() && params->is_array())
        for (const auto &param : *params) {
            if (!param.is_object()) continue;
            SurfaceParam entry;
            entry.name = text(param, "name");
            entry.type = text(param, "type");
            entry.default_value = param.value("default", Json{});
            entry.min = number(param, "min");
            entry.max = number(param, "max");
            entry.hint = text(param, "hint");
            entry.encoding = text(param, "encoding");
            if (!entry.name.empty()) schema.params.push_back(std::move(entry));
        }
    if (const auto textures = result.find("textures"); textures != result.end() && textures->is_array())
        for (const auto &texture : *textures) {
            if (!texture.is_object()) continue;
            SurfaceTexture entry;
            entry.name = text(texture, "name");
            entry.default_reference = text(texture, "default");
            entry.color_space = text(texture, "color_space");
            entry.role = text(texture, "role");
            entry.dimension = text(texture, "dimension");
            if (entry.dimension.empty()) entry.dimension = "2d";
            if (!entry.name.empty()) schema.textures.push_back(std::move(entry));
        }
    return schema;
}

MaterialFieldKind materialFieldKind(const SurfaceParam &param) {
    if (param.type == "color") return MaterialFieldKind::Color;
    if ((param.type == "vec3" || param.type == "vec4") && param.hint == "color") return MaterialFieldKind::Color;
    if (param.type == "float") return param.min && param.max ? MaterialFieldKind::Slider : MaterialFieldKind::Number;
    if (param.type == "int")
        return param.hint == "bool" || param.hint == "toggle" ? MaterialFieldKind::Toggle : MaterialFieldKind::Integer;
    if (param.type == "vec2" || param.type == "vec3" || param.type == "vec4") return MaterialFieldKind::Vector;
    return MaterialFieldKind::Other;
}

Json materialParamValue(const Json &material, const SurfaceParam &param) {
    if (material.is_object())
        if (const auto values = material.find("values"); values != material.end() && values->is_object())
            if (const auto found = values->find(param.name); found != values->end() && !found->is_null())
                return *found;
    return param.default_value;
}

bool materialParamOverridden(const Json &material, const SurfaceParam &param) {
    if (!material.is_object()) return false;
    const auto values = material.find("values");
    if (values == material.end() || !values->is_object()) return false;
    const auto found = values->find(param.name);
    if (found == values->end() || found->is_null()) return false;
    // 0.5 and 0.50000 are one value: compare as numbers where they are.
    const auto same = [](const Json &left, const Json &right) {
        if (left.is_number() && right.is_number()) return std::abs(left.get<double>() - right.get<double>()) < 1e-9;
        if (left.is_array() && right.is_array() && left.size() == right.size()) {
            for (std::size_t index = 0; index < left.size(); ++index)
                if (!left[index].is_number() || !right[index].is_number() ||
                    std::abs(left[index].get<double>() - right[index].get<double>()) >= 1e-9)
                    return false;
            return true;
        }
        return left == right;
    };
    return !same(*found, param.default_value);
}

std::string materialTextureValue(const Json &material, std::string_view texture) {
    if (!material.is_object()) return {};
    const auto textures = material.find("textures");
    if (textures == material.end() || !textures->is_object()) return {};
    return text(*textures, std::string{texture}.c_str());
}

double srgbFromLinear(double linear) {
    if (linear <= 0.0) return 0.0;
    if (linear >= 1.0) return 1.0;
    return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

double linearFromSrgb(double srgb) {
    if (srgb <= 0.0) return 0.0;
    if (srgb >= 1.0) return 1.0;
    return srgb <= 0.04045 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
}

std::array<double, 4> materialColorForDisplay(const Json &value, const SurfaceParam &param) {
    std::array<double, 4> shown{0.0, 0.0, 0.0, 1.0};
    if (!value.is_array()) return shown;
    const bool linear = param.encoding != "srgb";
    for (std::size_t index = 0; index < 4 && index < value.size(); ++index) {
        if (!value[index].is_number()) continue;
        const double component = std::clamp(value[index].get<double>(), 0.0, 1.0);
        shown[index] = index < 3 && linear ? srgbFromLinear(component) : component;
    }
    return shown;
}

Json materialColorFromDisplay(const std::array<double, 4> &shown, const SurfaceParam &param) {
    const bool linear = param.encoding != "srgb";
    const std::size_t components =
        param.default_value.is_array() && param.default_value.size() >= 4 ? 4 : (param.type == "vec4" ? 4 : 3);
    Json value = Json::array();
    for (std::size_t index = 0; index < components; ++index) {
        const double component = std::clamp(shown[index], 0.0, 1.0);
        value.push_back(rounded(index < 3 && linear ? linearFromSrgb(component) : component));
    }
    return value;
}

// --- list_surfaces

std::vector<SurfaceChoice> parseSurfaceList(const Json &result) {
    std::vector<SurfaceChoice> surfaces;
    if (!result.is_object()) return surfaces;
    const auto listed = result.find("surfaces");
    if (listed == result.end() || !listed->is_array()) return surfaces;
    for (const auto &surface : *listed) {
        if (!surface.is_object()) continue;
        SurfaceChoice entry;
        entry.reference = text(surface, "reference");
        entry.source = text(surface, "source");
        entry.name = text(surface, "name");
        entry.params = counter(surface, "params");
        entry.textures = counter(surface, "textures");
        entry.routing = surface.value("routing", Json{});
        if (const auto error = surface.find("error"); error != surface.end() && !error->is_null())
            entry.error = error->is_string() ? error->get<std::string>() : error->dump();
        if (!entry.reference.empty()) surfaces.push_back(std::move(entry));
    }
    return surfaces;
}

std::string surfaceChoiceLabel(const SurfaceChoice &surface) {
    std::string label = surface.reference;
    if (const auto scheme = label.find("://"); scheme != std::string::npos) label.erase(0, scheme + 3);
    constexpr std::string_view folder = "surfaces/";
    if (const auto at = label.rfind(folder); at != std::string::npos) label.erase(0, at + folder.size());
    constexpr std::string_view suffix = ".surface";
    if (label.size() > suffix.size() && label.ends_with(suffix)) label.erase(label.size() - suffix.size());
    if (surface.name.empty()) return label;
    // A named variant of a family: "openpbr / opaque_single_sided".
    const auto slash = label.rfind('/');
    return slash == std::string::npos ? surface.name : label.substr(0, slash) + " / " + surface.name;
}

std::optional<std::size_t> defaultSurfaceChoice(const std::vector<SurfaceChoice> &surfaces) {
    std::optional<std::size_t> first;
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        if (!surfaces[index].error.empty()) continue;
        if (surfaces[index].reference == "engine://surfaces/openpbr/opaque_single.surface") return index;
        if (!first) first = index;
    }
    return first;
}

// --- a new material's name

std::string materialDocumentPath(std::string_view name) {
    return "materials/" + std::string{name} + ".material.json";
}

bool validMaterialName(std::string_view name) {
    return !name.empty() && std::ranges::all_of(name, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    });
}

std::string suggestMaterialName(const MaterialList &list, const std::filesystem::path &project_root,
                                std::string_view base) {
    std::string stem{base.empty() ? std::string_view{"material"} : base};
    for (char &c : stem)
        if (!validMaterialName(std::string_view{&c, 1})) c = '_';
    const auto names = materialNames(list);
    std::set<std::string> paths;
    for (const auto &document : list.documents) paths.insert(document.path);
    const auto taken = [&](const std::string &candidate) {
        if (std::ranges::find(names, candidate) != names.end()) return true;
        const auto path = materialDocumentPath(candidate);
        if (paths.contains(path)) return true;
        if (project_root.empty()) return false;
        std::error_code error;
        return std::filesystem::exists(project_root / std::filesystem::path{path}, error);
    };
    if (!taken(stem)) return stem;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        auto candidate = stem + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
    return stem;
}

// --- texture slots

std::string textureFileDimension(const std::filesystem::path &file) {
    const auto extension = lower(file.extension().string());
    static const std::set<std::string> flat{".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".exr"};
    if (flat.contains(extension)) return "2d";
    if (extension != ".ktx2") return {};
    std::ifstream input{file, std::ios::binary};
    unsigned char header[40]{};
    if (!input.read(reinterpret_cast<char *>(header), sizeof header)) return {};
    static constexpr unsigned char identifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    if (!std::equal(std::begin(identifier), std::end(identifier), header)) return {};
    const auto word = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(header[offset]) | (static_cast<std::uint32_t>(header[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(header[offset + 2]) << 16) |
               (static_cast<std::uint32_t>(header[offset + 3]) << 24);
    };
    const auto depth = word(28);
    const auto layers = word(32);
    const auto faces = word(36);
    if (depth > 0) return "3d";
    if (faces == 6) return layers > 0 ? "cube_array" : "cube";
    if (layers > 0) return "2d_array";
    return "2d";
}

std::vector<std::string> textureChoices(const std::vector<AssetEntry> &entries, std::string_view dimension) {
    std::vector<const AssetEntry *> files;
    flattenFiles(entries, files);
    std::vector<std::string> choices;
    for (const auto *file : files)
        if (textureFileDimension(file->physical) == dimension) choices.push_back("project://" + file->logical);
    std::ranges::sort(choices);
    return choices;
}

// --- assignment

std::vector<MaterialPrimitive> parseMaterialBindings(const Json &result) {
    std::vector<MaterialPrimitive> primitives;
    if (!result.is_object()) return primitives;
    const auto listed = result.find("primitives");
    if (listed == result.end() || !listed->is_array()) return primitives;
    for (const auto &primitive : *listed) {
        if (!primitive.is_object()) continue;
        primitives.push_back({counter(primitive, "mesh"), counter(primitive, "primitive"), text(primitive, "slot"),
                              text(primitive, "material")});
    }
    return primitives;
}

std::optional<std::size_t> countModelUsers(const Json &scene, std::string_view model) {
    if (!scene.is_object() && !scene.is_array()) return std::nullopt;
    std::size_t count = 0;
    countUsers(scene, model, count);
    return count;
}

std::string modelScopeLine(std::string_view model, std::optional<std::size_t> objects) {
    std::string shown = builtinModelLabel(model);
    constexpr std::string_view builtin = " (built-in)";
    if (shown.ends_with(builtin)) shown.erase(shown.size() - builtin.size());
    if (shown.empty()) shown = std::string{model};
    std::string line = "Applies to every " + shown + " in this project";
    if (objects)
        line += " (" + std::to_string(*objects) + (*objects == 1 ? " object)" : " objects)");
    return line + ".";
}

// --- the Materials dock

MaterialsDockSession::MaterialsDockSession(Transport transport) : transport_{std::move(transport)} {}

void MaterialsDockSession::notify() {
    if (changed) changed();
}

void MaterialsDockSession::setNotice(std::string line, std::string detail) {
    notice_ = std::move(line);
    notice_detail_ = std::move(detail);
}

std::int64_t MaterialsDockSession::send(const std::string &method, const Json &params) {
    if (!transport_.request) return 0;
    std::string error;
    const auto id = transport_.request(method, params, &error);
    if (id == 0 && !error.empty()) setNotice("Could not ask the engine (" + method + "): " + error, error);
    return id;
}

void MaterialsDockSession::forgetRequests() {
    list_request_ = 0;
    material_request_ = 0;
    schema_request_ = 0;
    surfaces_request_ = 0;
    write_ = 0;
    in_flight_.reset();
    queue_.clear();
}

void MaterialsDockSession::open(std::filesystem::path project_root) {
    forgetRequests();
    root_ = std::move(project_root);
    ready_ = false;
    listed_ = false;
    list_ = {};
    digests_.clear();
    selection_.reset();
    select_when_listed_.reset();
    material_ = nullptr;
    schema_.reset();
    schema_surface_.clear();
    surfaces_.clear();
    surfaces_state_ = SurfacesState::Unknown;
    setNotice({});
    field_error_param_.clear();
    field_error_.clear();
    restart_reason_.clear();
    undo_.clear();
    notify();
}

void MaterialsDockSession::engineReady() {
    forgetRequests();
    ready_ = true;
    restart_reason_.clear();
    readList();
    readSurfaces();
    if (selection_) readSelected();
    notify();
}

void MaterialsDockSession::engineGone() {
    forgetRequests();
    ready_ = false;
    notify();
}

void MaterialsDockSession::poll() {
    if (ready_ && list_request_ == 0) readList();
}

void MaterialsDockSession::readList() {
    if (!ready_ || list_request_ != 0) return;
    list_request_ = send("list_materials", Json::object());
}

void MaterialsDockSession::readSurfaces() {
    if (!ready_ || surfaces_request_ != 0 || surfaces_state_ == SurfacesState::Unavailable) return;
    surfaces_request_ = send("list_surfaces", Json::object());
    if (surfaces_request_ != 0 && surfaces_state_ == SurfacesState::Unknown) surfaces_state_ = SurfacesState::Reading;
}

void MaterialsDockSession::readSelected() {
    if (!ready_ || !selection_) return;
    Json params{{"path", selection_->path}};
    if (!selection_->name.empty()) params["name"] = selection_->name;
    material_request_ = send("get_material", params);
}

void MaterialsDockSession::readSchema(const std::string &surface) {
    if (!ready_ || surface.empty()) return;
    schema_surface_ = surface;
    schema_request_ = send("get_surface_schema", Json{{"surface", surface}});
}

void MaterialsDockSession::select(const Selection &selection) {
    if (selection_ && *selection_ == selection) return;
    selection_ = selection;
    material_ = nullptr;
    schema_.reset();
    schema_surface_.clear();
    field_error_param_.clear();
    field_error_.clear();
    readSelected();
    notify();
}

void MaterialsDockSession::clearSelection() {
    if (!selection_) return;
    selection_.reset();
    material_ = nullptr;
    schema_.reset();
    schema_surface_.clear();
    material_request_ = 0;
    schema_request_ = 0;
    notify();
}

std::string MaterialsDockSession::digestOf(const std::string &path) const {
    const auto found = digests_.find(path);
    return found == digests_.end() ? std::string{} : found->second;
}

bool MaterialsDockSession::enqueue(Write write) {
    if (!ready_) return false;
    setNotice({});
    queue_.push_back(std::move(write));
    if (write_ == 0) sendNext();
    notify();
    return true;
}

void MaterialsDockSession::sendNext() {
    while (write_ == 0 && !queue_.empty()) {
        auto write = std::move(queue_.front());
        queue_.pop_front();
        auto params = write.params;
        if (write.kind == Write::Kind::Edit || write.kind == Write::Kind::UndoEdit) {
            params["path"] = write.path;
            if (!write.name.empty()) params["name"] = write.name;
            params["base_material_digest"] = digestOf(write.path);
        }
        write_ = send(write.method, params);
        if (write_ != 0) in_flight_ = std::move(write);
    }
}

bool MaterialsDockSession::editSelected(Json params, std::string description, std::string param) {
    if (!selection_ || !material_.is_object()) return false;
    Write write;
    write.kind = Write::Kind::Edit;
    write.method = "edit_material";
    write.path = selection_->path;
    write.name = selection_->name;
    write.param = std::move(param);
    write.description = std::move(description);
    write.params = std::move(params);
    return enqueue(std::move(write));
}

bool MaterialsDockSession::setValue(const std::string &param, const Json &value) {
    return editSelected(Json{{"values", Json{{param, value}}}},
                        value.is_null() ? param + " back to its default" : param, param);
}

bool MaterialsDockSession::setTexture(const std::string &texture, const Json &reference) {
    return editSelected(Json{{"textures", Json{{texture, reference}}}},
                        reference.is_null() ? texture + " back to its default" : texture, texture);
}

bool MaterialsDockSession::setTags(const std::vector<std::string> &tags) {
    return editSelected(Json{{"tags", tags.empty() ? Json(nullptr) : Json(tags)}}, "tags");
}

bool MaterialsDockSession::setRouting(const std::string &alpha_mode, bool double_sided) {
    return editSelected(Json{{"routing", Json{{"alpha_mode", alpha_mode}, {"double_sided", double_sided}}}},
                        "routing");
}

bool MaterialsDockSession::setSurface(const std::string &surface) {
    if (surface.empty()) return false;
    return editSelected(Json{{"surface", surface}}, "surface");
}

bool MaterialsDockSession::create(const SurfaceChoice &surface, const std::string &name) {
    if (!surface.error.empty() || surface.reference.empty()) return false;
    if (!validMaterialName(name)) {
        setNotice("A material name is letters, digits and _.");
        notify();
        return false;
    }
    Write write;
    write.kind = Write::Kind::Create;
    write.method = "create_material";
    write.path = materialDocumentPath(name);
    write.name = name;
    write.description = "New material " + name;
    write.params = Json{{"path", write.path}, {"surface", surface.reference}, {"name", name}};
    if (surface.routing.is_object()) write.params["routing"] = surface.routing;
    return enqueue(std::move(write));
}

bool MaterialsDockSession::assign(const std::string &model, const std::string &object) {
    if (!selection_ || model.empty() || selection_->name.empty()) return false;
    Write write;
    write.kind = Write::Kind::Bind;
    write.method = "bind_material";
    write.model = model;
    write.name = selection_->name;
    write.description = selection_->name + " on " + (object.empty() ? model : object);
    write.params = Json{{"model", model}, {"material", selection_->name}};
    return enqueue(std::move(write));
}

bool MaterialsDockSession::undo() {
    if (undo_.empty() || editPending() || !ready_) return false;
    const auto &entry = undo_.back();
    Write write;
    write.description = entry.description;
    if (entry.kind == UndoEntry::Kind::Edit) {
        write.kind = Write::Kind::UndoEdit;
        write.method = "edit_material";
        write.path = entry.path;
        write.name = entry.name;
        write.params = entry.previous.is_object() ? entry.previous : Json::object();
    } else {
        write.kind = Write::Kind::UndoBind;
        write.method = "bind_material";
        write.model = entry.model;
        write.params = Json{{"model", entry.model}, {"bindings", entry.previous}};
    }
    return enqueue(std::move(write));
}

void MaterialsDockSession::say(std::string line) {
    setNotice(std::move(line));
    notify();
}

bool MaterialsDockSession::owns(std::int64_t id) const noexcept {
    return id != 0 && (id == list_request_ || id == material_request_ || id == schema_request_ ||
                       id == surfaces_request_ || id == write_);
}

void MaterialsDockSession::listResult(const Json &result) {
    auto list = parseMaterialList(result);
    const bool first = !listed_;
    listed_ = true;
    // What this dock last knew of the shown document: its own writes and
    // reads move it, so only a digest it did not see is someone else's.
    const auto known = selection_ ? digestOf(selection_->path) : std::string{};
    for (const auto &document : list.documents)
        if (!document.digest.empty()) digests_[document.path] = document.digest;
    if (select_when_listed_) {
        const auto wanted = *select_when_listed_;
        if (materialDocumentOf(list, wanted.name) != nullptr) {
            select_when_listed_.reset();
            list_ = std::move(list);
            if (listChanged) listChanged();
            select(wanted);
            return;
        }
    }
    if (!first && list == list_) return;
    // The document of the material shown was written by someone else: read
    // it again (a write of this dock reads it on its own reply).
    bool reread = false;
    if (selection_ && write_ == 0) {
        const auto after = std::ranges::find(list.documents, selection_->path, &MaterialListDocument::path);
        if (after == list.documents.end()) {
            list_ = std::move(list);
            clearSelection();
            if (listChanged) listChanged();
            notify();
            return;
        }
        reread = after->digest != known;
    }
    list_ = std::move(list);
    if (reread && material_request_ == 0) readSelected();
    if (listChanged) listChanged();
    notify();
}

void MaterialsDockSession::writeResult(const Write &write, const Json &result) {
    const bool changed_now = result.value("changed", true);
    switch (write.kind) {
    case Write::Kind::Edit:
    case Write::Kind::UndoEdit: {
        if (const auto digest = text(result, "material_digest"); !digest.empty()) digests_[write.path] = digest;
        if (write.kind == Write::Kind::UndoEdit) {
            if (!undo_.empty()) undo_.pop_back();
        } else if (changed_now) {
            UndoEntry entry;
            entry.kind = UndoEntry::Kind::Edit;
            entry.description = write.description;
            entry.path = write.path;
            entry.name = text(result, "name").empty() ? write.name : text(result, "name");
            entry.previous = result.value("previous", Json::object());
            undo_.push_back(std::move(entry));
        }
        if (!write.param.empty() && write.param == field_error_param_) {
            field_error_param_.clear();
            field_error_.clear();
        }
        if (selection_ && selection_->path == write.path) readSelected();
        break;
    }
    case Write::Kind::Create:
        setNotice("Created " + text(result, "name") + " in " + text(result, "path") + ".");
        select_when_listed_ = Selection{text(result, "path").empty() ? write.path : text(result, "path"),
                                        text(result, "name").empty() ? write.name : text(result, "name")};
        if (const auto digest = text(result, "material_digest"); !digest.empty()) digests_[write.path] = digest;
        break;
    case Write::Kind::Bind:
    case Write::Kind::UndoBind: {
        if (write.kind == Write::Kind::UndoBind) {
            if (!undo_.empty()) undo_.pop_back();
        } else {
            const auto previous = result.value("previous", Json::object()).value("bindings", Json::array());
            if (changed_now && previous.is_array() && !previous.empty()) {
                UndoEntry entry;
                entry.kind = UndoEntry::Kind::Bind;
                entry.description = write.description;
                entry.model = write.model;
                entry.previous = previous;
                undo_.push_back(std::move(entry));
            }
        }
        if (bound) bound(write.model);
        break;
    }
    }
    const auto restart = result.find("restart_required");
    if (restart != result.end() && !restart->is_null()) {
        restart_reason_ = restart->is_string() ? restart->get<std::string>() : text(*restart, "reason");
        if (restart_reason_.empty()) restart_reason_ = "the engine reads it when it starts";
        if (restartRequested) restartRequested(restart_reason_);
    }
    readList();
}

bool MaterialsDockSession::receiveResult(std::int64_t id, const Json &result) {
    if (!owns(id)) return false;
    if (id == list_request_) {
        list_request_ = 0;
        listResult(result);
        return true;
    }
    if (id == material_request_) {
        material_request_ = 0;
        material_ = result.is_object() ? result.value("material", Json{}) : Json{};
        if (const auto digest = text(result, "material_digest"); !digest.empty() && selection_)
            digests_[selection_->path] = digest;
        if (selection_ && selection_->name.empty()) selection_->name = text(result, "name");
        const auto surface = text(material_, "surface");
        if (surface != schema_surface_) {
            schema_.reset();
            readSchema(surface);
        }
    } else if (id == schema_request_) {
        schema_request_ = 0;
        schema_ = parseSurfaceSchema(result);
    } else if (id == surfaces_request_) {
        surfaces_request_ = 0;
        surfaces_ = parseSurfaceList(result);
        surfaces_state_ = SurfacesState::Listed;
    } else if (id == write_) {
        write_ = 0;
        if (in_flight_) {
            const auto write = std::move(*in_flight_);
            in_flight_.reset();
            writeResult(write, result);
        }
        sendNext();
    }
    notify();
    return true;
}

bool MaterialsDockSession::receiveRefusal(std::int64_t id, int code, const std::string &error_code,
                                          const std::string &message, const Json &error_fields) {
    if (!owns(id)) return false;
    if (id == surfaces_request_ && code == -32601) {
        surfaces_request_ = 0;
        surfaces_state_ = SurfacesState::Unavailable;
        surfaces_.clear();
        notify();
        return true;
    }
    if (error_code.empty()) return receiveFailure(id, message);
    const auto sentence = sentenceFor(error_code, message, error_fields);
    if (id == write_) {
        write_ = 0;
        const auto write = in_flight_ ? std::move(*in_flight_) : Write{};
        in_flight_.reset();
        // What waited behind it was made against what was refused.
        queue_.clear();
        if (error_code == "material.value_invalid@1") {
            field_error_param_ = error_fields.is_object() ? text(error_fields, "param") : std::string{};
            if (field_error_param_.empty()) field_error_param_ = write.param;
            field_error_ = sentence;
        } else {
            setNotice(sentence, message);
        }
        // The digest it was written against is old, or the file is not what
        // the editor shows: read both again.
        readList();
        if (selection_ && material_request_ == 0) readSelected();
    } else {
        if (id == list_request_) list_request_ = 0;
        else if (id == material_request_) material_request_ = 0;
        else if (id == schema_request_) schema_request_ = 0;
        else if (id == surfaces_request_) {
            surfaces_request_ = 0;
            surfaces_state_ = SurfacesState::Unavailable;
        }
        setNotice(sentence, message);
    }
    notify();
    return true;
}

bool MaterialsDockSession::receiveFailure(std::int64_t id, const std::string &message) {
    if (!owns(id)) return false;
    if (id == list_request_) {
        list_request_ = 0;
    } else if (id == material_request_) {
        material_request_ = 0;
        setNotice("Could not read the material: " + message, message);
    } else if (id == schema_request_) {
        schema_request_ = 0;
        setNotice("Could not read the surface's parameters: " + message, message);
    } else if (id == surfaces_request_) {
        surfaces_request_ = 0;
        surfaces_state_ = SurfacesState::Unavailable;
    } else if (id == write_) {
        write_ = 0;
        in_flight_.reset();
        queue_.clear();
        setNotice("Could not change the material: " + message, message);
        readList();
    }
    notify();
    return true;
}

// --- the Inspector's Material rows

MaterialAssignSession::MaterialAssignSession(Transport transport) : transport_{std::move(transport)} {}

void MaterialAssignSession::notify() {
    if (changed) changed();
}

std::int64_t MaterialAssignSession::send(const std::string &method, const Json &params) {
    if (!transport_.request) return 0;
    std::string error;
    const auto id = transport_.request(method, params, &error);
    if (id == 0 && !error.empty()) {
        notice_ = "Could not ask the engine (" + method + "): " + error;
        notice_detail_ = error;
    }
    return id;
}

void MaterialAssignSession::engineReady() {
    ready_ = true;
    bindings_request_ = materials_request_ = count_request_ = bind_request_ = 0;
    readAll();
    notify();
}

void MaterialAssignSession::engineGone() {
    ready_ = false;
    bindings_request_ = materials_request_ = count_request_ = bind_request_ = 0;
    notify();
}

void MaterialAssignSession::setSubject(const std::string &model, const std::string &object) {
    object_ = object;
    if (model == model_) return;
    model_ = model;
    primitives_.clear();
    bindings_read_ = false;
    objects_.reset();
    notice_.clear();
    notice_detail_.clear();
    bindings_request_ = count_request_ = 0;
    readAll();
    notify();
}

void MaterialAssignSession::materialsChanged() { readMaterials(); }

void MaterialAssignSession::bindingsChanged() { readBindings(); }

void MaterialAssignSession::readAll() {
    if (!ready_ || model_.empty()) return;
    readMaterials();
    readBindings();
    readCount();
}

void MaterialAssignSession::readBindings() {
    if (!ready_ || model_.empty()) return;
    bindings_request_ = send("get_material_bindings", Json{{"model", model_}});
}

void MaterialAssignSession::readMaterials() {
    if (!ready_ || model_.empty()) return;
    materials_request_ = send("list_materials", Json::object());
}

void MaterialAssignSession::readCount() {
    if (!ready_ || model_.empty()) return;
    count_request_ = send("export_scene_snapshot", Json{{"schema_version", 2}, {"allow_pending", true}});
}

std::optional<std::string> MaterialAssignSession::sharedMaterial() const {
    if (primitives_.empty()) return std::nullopt;
    const bool own = std::ranges::all_of(primitives_, [](const auto &p) { return p.material == p.slot; });
    if (own) return std::string{};
    const auto &first = primitives_.front().material;
    if (std::ranges::all_of(primitives_, [&](const auto &p) { return p.material == first; })) return first;
    return std::nullopt;
}

std::optional<std::string> MaterialAssignSession::shownMaterial() const {
    if (bind_request_ != 0 && requested_) return requested_;
    return sharedMaterial();
}

bool MaterialAssignSession::bind(Json params, std::string description, bool undoing,
                                 std::optional<std::string> requested) {
    if (!ready_ || model_.empty() || bind_request_ != 0) return false;
    notice_.clear();
    notice_detail_.clear();
    bind_description_ = std::move(description);
    bind_undoing_ = undoing;
    requested_ = std::move(requested);
    bind_request_ = send("bind_material", params);
    notify();
    return bind_request_ != 0;
}

bool MaterialAssignSession::choose(const std::string &material) {
    if (!bindings_read_) return false;
    const auto label = object_.empty() ? model_ : object_;
    if (!material.empty())
        return bind(Json{{"model", model_}, {"material", material}}, material + " on " + label, false, material);
    auto bindings = Json::array();
    for (const auto &primitive : primitives_)
        if (primitive.material != primitive.slot)
            bindings.push_back(
                {{"mesh", primitive.mesh}, {"primitive", primitive.primitive}, {"material", primitive.slot}});
    if (bindings.empty()) return false;
    return bind(Json{{"model", model_}, {"bindings", bindings}}, "Model default on " + label, false,
                std::string{});
}

bool MaterialAssignSession::choosePrimitive(std::size_t index, const std::string &material) {
    if (index >= primitives_.size()) return false;
    const auto &primitive = primitives_[index];
    const auto chosen = material.empty() ? primitive.slot : material;
    if (chosen == primitive.material) return false;
    return bind(Json{{"model", model_},
                     {"bindings", Json::array({{{"mesh", primitive.mesh},
                                                {"primitive", primitive.primitive},
                                                {"material", chosen}}})}},
                (material.empty() ? std::string{"Model default"} : material) + " on mesh " +
                    std::to_string(primitive.mesh) + " primitive " + std::to_string(primitive.primitive),
                false);
}

bool MaterialAssignSession::undo() {
    if (undo_.empty()) return false;
    return bind(Json{{"model", model_}, {"bindings", undo_.back().bindings}}, undo_.back().description, true);
}

bool MaterialAssignSession::owns(std::int64_t id) const noexcept {
    return id != 0 &&
           (id == bindings_request_ || id == materials_request_ || id == count_request_ || id == bind_request_);
}

bool MaterialAssignSession::receiveResult(std::int64_t id, const Json &result) {
    if (!owns(id)) return false;
    if (id == bindings_request_) {
        bindings_request_ = 0;
        primitives_ = parseMaterialBindings(result);
        bindings_read_ = true;
    } else if (id == materials_request_) {
        materials_request_ = 0;
        materials_ = materialNames(parseMaterialList(result));
    } else if (id == count_request_) {
        count_request_ = 0;
        objects_.reset();
        if (result.is_object())
            if (const auto bytes = result.find("semantic_scene_bytes"); bytes != result.end() && bytes->is_string()) {
                const auto scene = Json::parse(bytes->get<std::string>(), nullptr, false);
                if (!scene.is_discarded()) objects_ = countModelUsers(scene, model_);
            }
    } else if (id == bind_request_) {
        bind_request_ = 0;
        if (bind_undoing_) {
            if (!undo_.empty()) undo_.pop_back();
        } else {
            const auto previous = result.value("previous", Json::object()).value("bindings", Json::array());
            if (result.value("changed", true) && previous.is_array() && !previous.empty())
                undo_.push_back({bind_description_, previous});
        }
        const auto restart = result.find("restart_required");
        if (restart != result.end() && !restart->is_null()) {
            auto reason = restart->is_string() ? restart->get<std::string>() : text(*restart, "reason");
            if (reason.empty()) reason = "the engine reads it when it starts";
            notice_ = "Restart the engine to apply: " + reason;
            notice_detail_ = reason;
            if (restartRequested) restartRequested(reason);
        }
        readBindings();
    }
    notify();
    return true;
}

bool MaterialAssignSession::receiveRefusal(std::int64_t id, int code, const std::string &error_code,
                                           const std::string &message, const Json &error_fields) {
    (void)code;
    if (!owns(id)) return false;
    if (error_code.empty()) return receiveFailure(id, message);
    if (id == bindings_request_) bindings_request_ = 0;
    else if (id == materials_request_) materials_request_ = 0;
    else if (id == count_request_) {
        // The count is said when it is known; not knowing it says nothing.
        count_request_ = 0;
        notify();
        return true;
    } else if (id == bind_request_) {
        bind_request_ = 0;
        readBindings();
    }
    notice_ = sentenceFor(error_code, message, error_fields);
    notice_detail_ = message;
    notify();
    return true;
}

bool MaterialAssignSession::receiveFailure(std::int64_t id, const std::string &message) {
    if (!owns(id)) return false;
    if (id == bindings_request_) {
        bindings_request_ = 0;
        notice_ = "Could not read the model's materials: " + message;
        notice_detail_ = message;
    } else if (id == materials_request_) {
        materials_request_ = 0;
    } else if (id == count_request_) {
        count_request_ = 0;
    } else if (id == bind_request_) {
        bind_request_ = 0;
        notice_ = "Could not bind the material: " + message;
        notice_detail_ = message;
        readBindings();
    }
    notify();
    return true;
}

} // namespace PelicanStudio
