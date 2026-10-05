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
        entry.pass = text(document, "pass");
        entry.requirement = text(document, "requirement");
        entry.line = static_cast<std::int64_t>(counter(document, "line"));
        entry.column = static_cast<std::int64_t>(counter(document, "column"));
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

// --- FO-1s

std::string materialPassContractSentence(const Json &fields) {
    if (!fields.is_object()) return {};
    std::vector<std::string> passes;
    if (const auto pass = text(fields, "pass"); !pass.empty()) passes.push_back(pass);
    if (const auto found = fields.find("passes"); passes.empty() && found != fields.end() && found->is_array())
        passes = strings(*found);
    if (passes.empty()) return {};
    std::string named;
    for (std::size_t index = 0; index < passes.size(); ++index)
        named += (index == 0 ? "" : index + 1 == passes.size() ? " and " : ", ") + passes[index];
    const auto material = text(fields, "material");
    const auto who = material.empty() ? std::string{"The material"} : "Material " + material;
    const auto requirement = text(fields, "requirement");
    const auto pass_word = passes.size() == 1 ? std::string{"pass "} : std::string{"passes "};
    if (requirement.empty() || requirement == "ray_query")
        return who + " queries the scene's acceleration structure, and " + pass_word + named +
               (passes.size() == 1 ? ", which draws it, does" : ", which draw it, do") +
               " not declare ray_query: turn Ray query on for " + (passes.size() == 1 ? "the pass" : "them");
    return who + " needs " + requirement + " of " + pass_word + named + ", which " +
           (passes.size() == 1 ? "does" : "do") + " not declare it";
}

std::string materialUnusableSentence(const Json &problem) {
    if (!problem.is_object()) return "The engine cannot draw the material yet.";
    if (auto sentence = materialPassContractSentence(problem); !sentence.empty()) {
        const auto code = text(problem, "code");
        return sentence + (code.empty() ? std::string{"."} : " (" + code + ").");
    }
    auto message = text(problem, "message");
    if (const auto newline = message.find('\n'); newline != std::string::npos) message.erase(newline);
    return message.empty() ? std::string{"The engine cannot draw the material yet."} : message;
}

std::string materialDocumentProblemLine(const MaterialListDocument &document) {
    if (document.status == "loaded" || document.status.empty()) return {};
    if (document.status == "unusable" && !document.pass.empty())
        return document.path + ": " +
               materialPassContractSentence(Json{{"pass", document.pass}, {"requirement", document.requirement}}) + ".";
    if (document.status == "missing")
        return document.path + " is not on disk" + (document.reason.empty() ? "" : ": " + document.reason);
    std::string line = document.path + " would not load";
    if (document.line > 0) {
        line += " at line " + std::to_string(document.line);
        if (document.column > 0) line += ", column " + std::to_string(document.column);
    }
    if (!document.reason.empty()) line += ": " + document.reason;
    return line;
}

std::string materialDeleteRefusalDescription(const std::string &error_code, const Json &fields, bool rename) {
    const auto joined = [](const std::vector<std::string> &names) {
        std::string line;
        for (std::size_t index = 0; index < names.size(); ++index)
            line += (index == 0 ? "" : index + 1 == names.size() ? " and " : ", ") + names[index];
        return line;
    };
    auto name = text(fields, "name");
    if (name.empty()) name = "The material";
    if (error_code == "material.in_use@1") {
        std::vector<std::string> places;
        std::vector<std::string> models;
        for (const auto &model : strings(fields.value("bindings", Json::array())))
            models.push_back(modelShortName(model));
        if (!models.empty()) places.push_back("the bindings of " + joined(models));
        const auto objects = strings(fields.value("objects", Json::array()));
        const auto count = std::max<std::uint64_t>(counter(fields, "count"), objects.size());
        if (count > 0) {
            std::string line = std::to_string(count) + (count == 1 ? " object" : " objects");
            if (!objects.empty()) {
                std::string names;
                for (const auto &object : objects) names += (names.empty() ? "" : ", ") + object;
                // The engine names the first 8.
                if (count > objects.size()) names += ", \xE2\x80\xA6";
                line += " (" + names + ")";
            }
            places.push_back(line);
        }
        const auto documents = strings(fields.value("render_documents", Json::array()));
        if (!documents.empty())
            places.push_back((documents.size() == 1 ? "render document " : "render documents ") + joined(documents));
        const auto where = places.empty() ? std::string{"the project"} : joined(places);
        return rename ? name + " is named by " + where + ", which a rename does not rewrite (material.in_use@1)."
                      : name + " is used by " + where + "; take it off them first (material.in_use@1).";
    }
    if (error_code == "material.document_shared@1") {
        const auto names = strings(fields.value("names", Json::array()));
        return text(fields, "path") + " holds " + (names.empty() ? std::string{"other materials"} : joined(names)) +
               ": only a document of one material is deleted, so " + name +
               " stays (material.document_shared@1).";
    }
    if (error_code == "material.name_invalid@1") {
        const auto reason = text(fields, "reason");
        if (reason == "engine_material")
            return name + " is the engine's own material; it is not " + (rename ? "renamed" : "deleted") +
                   " (material.name_invalid@1).";
        if (reason == "model_material")
            return name + " is a material of model " + text(fields, "model") + ", not the project's; it is not " +
                   (rename ? "renamed" : "deleted") + " (material.name_invalid@1).";
    }
    return {};
}

// --- MR-1s

std::string materialFileProblemLine(const MaterialFileProblem &problem) {
    std::string line = problem.code == "material.document_unknown@1"
                           ? std::string{"the file is not on disk; the engine keeps the last values"}
                           : std::string{MaterialFileStaleMark};
    if (problem.line > 0) {
        line += ": line " + std::to_string(problem.line);
        if (problem.column > 0) line += ", column " + std::to_string(problem.column);
    }
    if (!problem.reason.empty()) line += ": " + problem.reason;
    return line;
}

std::string materialRegisterRefusalLine(const std::string &error_code, const std::string &message,
                                        const Json &fields) {
    // "[json.exception.parse_error.101] parse error at line L, column C: <what>"
    auto reason = text(fields, "reason");
    constexpr std::string_view at_line = "parse error at line ";
    const auto at = reason.find(at_line);
    if (error_code != "material.document_invalid@1" || at == std::string::npos)
        return sentenceFor(error_code, message, fields);
    std::size_t cursor = at + at_line.size();
    const auto digits = [&] {
        std::string value;
        while (cursor < reason.size() && std::isdigit(static_cast<unsigned char>(reason[cursor])) != 0)
            value += reason[cursor++];
        return value;
    };
    const auto line = digits();
    std::string column;
    if (reason.compare(cursor, 9, ", column ") == 0) {
        cursor += 9;
        column = digits();
    }
    if (reason.compare(cursor, 2, ": ") == 0) cursor += 2;
    auto path = text(fields, "path");
    if (path.empty()) path = "The document";
    return path + " would not load at line " + line + (column.empty() ? "" : ", column " + column) + ": " +
           reason.substr(cursor) + " (" + error_code + ").";
}

std::string materialLoadedAgainLine(const std::vector<std::string> &names, const std::string &path) {
    if (names.empty()) return path + " is loaded again.";
    std::string listed;
    for (const auto &name : names) listed += (listed.empty() ? "" : ", ") + name;
    return listed + (names.size() == 1 ? " is" : " are") + " loaded again.";
}

std::vector<std::string> materialNamesInText(std::string_view bytes) {
    const auto document = Json::parse(bytes, nullptr, false);
    std::vector<std::string> names;
    if (!document.is_object()) return names;
    const auto materials = document.find("materials");
    if (materials == document.end() || !materials->is_array()) return names;
    for (const auto &material : *materials)
        if (const auto name = text(material, "name"); !name.empty()) names.push_back(name);
    return names;
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
        if (const auto routings = surface.find("routings"); routings != surface.end() && routings->is_array())
            for (const auto &routing : *routings)
                if (routing.is_object()) entry.routings.push_back(routing);
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

std::string materialRoutingLabel(const Json &routing) {
    if (!routing.is_object()) return {};
    return routing.value("alpha_mode", std::string{"opaque"}) +
           (routing.value("double_sided", false) ? ", double-sided" : ", one-sided");
}

std::vector<Json> materialRoutingChoices(const SurfaceChoice &surface) {
    // A variant of the engine's manifest: its routing picks the variant of
    // the family (VR-1), so every routing is one the engine pairs.
    if (!surface.name.empty()) {
        std::vector<Json> every;
        for (const char *mode : {"opaque", "mask", "blend"})
            for (const bool sides : {false, true}) every.push_back(Json{{"alpha_mode", mode}, {"double_sided", sides}});
        return every;
    }
    if (!surface.routings.empty()) return surface.routings;
    if (surface.routing.is_object()) return {surface.routing};
    return {};
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
        MaterialPrimitive entry{counter(primitive, "mesh"), counter(primitive, "primitive"), text(primitive, "slot"),
                                text(primitive, "material")};
        if (const auto unresolved = primitive.find("unresolved");
            unresolved != primitive.end() && unresolved->is_object()) {
            entry.unresolved = true;
            entry.unresolved_reason = text(*unresolved, "reason");
            entry.drawn = text(*unresolved, "drawn");
            if (entry.drawn.empty()) entry.drawn = entry.slot;
        }
        primitives.push_back(std::move(entry));
    }
    return primitives;
}

std::string materialBindingUnresolvedLine(const std::vector<MaterialPrimitive> &primitives) {
    for (const auto &primitive : primitives)
        if (primitive.unresolved)
            return primitive.material + " is not registered \xE2\x80\x94 drawing " +
                   (primitive.drawn.empty() ? primitive.slot : primitive.drawn);
    return {};
}

ModelObjectCounts parseModelObjectCounts(const Json &result) {
    ModelObjectCounts counts;
    if (!result.is_object()) return counts;
    const auto count = [&](const char *key) -> std::optional<std::size_t> {
        const auto found = result.find(key);
        if (found == result.end() || !found->is_number_integer() || found->get<std::int64_t>() < 0)
            return std::nullopt;
        return static_cast<std::size_t>(found->get<std::int64_t>());
    };
    counts.objects = count("objects");
    counts.overrides = count("object_overrides");
    return counts;
}

std::string modelShortName(std::string_view model) {
    std::string shown = builtinModelLabel(model);
    constexpr std::string_view builtin = " (built-in)";
    if (shown.ends_with(builtin)) shown.erase(shown.size() - builtin.size());
    if (shown.empty()) shown = std::string{model};
    return shown;
}

std::string modelScopeLine(std::string_view model, const ModelObjectCounts &counts) {
    std::string line = "Applies to every " + modelShortName(model) + " in this project";
    if (counts.objects) {
        line += " (" + std::to_string(*counts.objects) + (*counts.objects == 1 ? " object" : " objects");
        if (counts.overrides) line += ", " + std::to_string(*counts.overrides) + " with their own material";
        line += ")";
    }
    return line + ".";
}

std::string objectScopeLine(std::string_view object) {
    return "Applies to " + (object.empty() ? std::string{"this object"} : std::string{object}) + " only.";
}

// --- an object's own materials

ObjectMaterials parseObjectMaterials(const Json &result) {
    ObjectMaterials materials;
    if (!result.is_object()) return materials;
    materials.object = counter(result, "object");
    materials.scene_id = text(result, "scene_id");
    materials.model = text(result, "model");
    if (const auto rows = result.find("materials"); rows != result.end() && rows->is_array())
        for (const auto &row : *rows) {
            if (!row.is_object()) continue;
            ObjectMaterialRow parsed{.row_id = counter(row, "row_id"), .material = text(row, "material")};
            if (const auto primitive = row.find("primitive"); primitive != row.end() && primitive->is_object())
                parsed.primitive = std::pair{counter(*primitive, "mesh"), counter(*primitive, "primitive")};
            if (const auto resolved = row.find("resolved"); resolved != row.end() && resolved->is_boolean())
                parsed.resolved = resolved->get<bool>();
            materials.rows.push_back(std::move(parsed));
        }
    if (const auto primitives = result.find("primitives"); primitives != result.end() && primitives->is_array())
        for (const auto &primitive : *primitives) {
            if (!primitive.is_object()) continue;
            materials.primitives.push_back({.mesh = counter(primitive, "mesh"),
                                            .primitive = counter(primitive, "primitive"),
                                            .slot = text(primitive, "slot"),
                                            .material = text(primitive, "material"),
                                            .source = text(primitive, "source"),
                                            .model_material = text(primitive, "model_material"),
                                            .drawn = text(primitive, "drawn")});
        }
    return materials;
}

std::string objectMaterialLabel(const ObjectMaterialPrimitive &primitive) {
    const auto &shown = primitive.drawn.empty() ? primitive.material : primitive.drawn;
    if (primitive.source == "object") return shown + " (this object)";
    return "(model default: " + shown + ")";
}

std::string objectMaterialMissingLine(const ObjectMaterials &materials) {
    for (const auto &row : materials.rows)
        if (!row.resolved) return "Material " + row.material + " is missing; drawing the model default";
    return {};
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
    recheck_.reset();
    broken_when_listed_.reset();
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
    broken_.reset();
    uses_.reset();
    repair_.reset();
    problems_ = {};
    recheck_.reset();
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
    // MR-1s: a register_material that will not be answered.
    if (repair_ && repair_->state == Repair::State::Waiting && !repair_->watch) {
        repair_->state = Repair::State::Refused;
        repair_->sentence = "The engine stopped; " + repair_->path + " is read when it starts again.";
    }
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
    if (selection_ && *selection_ == selection && !broken_) return;
    broken_.reset();
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
    uses_.reset();
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

bool MaterialsDockSession::create(const SurfaceChoice &surface, const std::string &name, const Json &routing) {
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
    if (routing.is_object()) write.params["routing"] = routing;
    else if (surface.routing.is_object()) write.params["routing"] = surface.routing;
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
    } else if (entry.kind == UndoEntry::Kind::Delete) {
        // FO-1s, decision 3: the document delete_material took off.
        write.kind = Write::Kind::UndoDelete;
        write.method = "register_material";
        write.path = entry.path;
        write.name = entry.name;
        write.params = Json{{"path", entry.path}};
    } else if (entry.kind == UndoEntry::Kind::Rename) {
        write.kind = Write::Kind::UndoRename;
        write.method = "rename_material";
        write.path = entry.path;
        write.name = entry.previous.is_string() ? entry.previous.get<std::string>() : std::string{};
        write.params = Json{{"name", entry.name}, {"new_name", entry.previous}};
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

const MaterialListDocument *MaterialsDockSession::documentOf(const std::string &path) const {
    const auto found = std::ranges::find(list_.documents, path, &MaterialListDocument::path);
    return found == list_.documents.end() ? nullptr : &*found;
}

void MaterialsDockSession::selectBroken(const std::string &path) {
    if (broken_ && *broken_ == path && !selection_) return;
    selection_.reset();
    material_ = nullptr;
    schema_.reset();
    schema_surface_.clear();
    material_request_ = 0;
    schema_request_ = 0;
    field_error_param_.clear();
    field_error_.clear();
    broken_ = path;
    notify();
}

bool MaterialsDockSession::registerAgain(const std::string &path) {
    if (path.empty()) return false;
    Write write;
    write.kind = Write::Kind::Register;
    write.method = "register_material";
    write.path = path;
    write.description = "Register " + path;
    write.params = Json{{"path", path}};
    return enqueue(std::move(write));
}

bool MaterialsDockSession::reloadFromFile(const std::string &path) {
    if (path.empty()) return false;
    Write write;
    write.kind = Write::Kind::Register;
    write.method = "register_material";
    write.path = path;
    write.description = "Reload " + path + " from its file";
    write.params = Json{{"path", path}};
    write.from_file = true;
    return enqueue(std::move(write));
}

void MaterialsDockSession::documentSaved(const std::string &path, std::vector<std::string> names,
                                         std::string digest) {
    Repair repair;
    repair.path = path;
    repair.names = std::move(names);
    repair.saved_digest = std::move(digest);
    if (const auto *stale = staleOf(path)) {
        // Decision 3: registered, and refused by the engine's watch after its
        // start -- the watch reads the saved file; nothing is sent.
        repair.watch = true;
        repair.stale_line = materialFileProblemLine(*stale);
        repair_ = std::move(repair);
        setNotice("Saved " + path + "; the engine's watch takes it in.");
        notify();
        return;
    }
    // Decision 1: register_material (engine EB-2's recovered).
    repair_ = std::move(repair);
    if (!registerAgain(path)) {
        repair_->state = Repair::State::Refused;
        repair_->sentence = "The engine is not running; " + path + " is read when it starts.";
        setNotice(repair_->sentence);
    } else {
        setNotice("Saved " + path + "; the engine takes it in.");
    }
    notify();
}

const MaterialFileProblem *MaterialsDockSession::staleOf(const std::string &path) const {
    const auto found = std::ranges::find(problems_.stale, path, &MaterialFileProblem::path);
    return found == problems_.stale.end() ? nullptr : &*found;
}

void MaterialsDockSession::loadedAgain(const std::string &path, std::vector<std::string> names) {
    if (names.empty() && repair_ && repair_->path == path) names = repair_->names;
    if (names.empty())
        if (const auto *listed = documentOf(path))
            for (const auto &material : listed->materials) names.push_back(material.name);
    setNotice(materialLoadedAgainLine(names, path));
    restart_reason_.clear();
    if (repair_ && repair_->path == path && repair_->state == Repair::State::Waiting)
        repair_->state = Repair::State::Loaded;
    if (problemsMayHaveChanged) problemsMayHaveChanged();
}

void MaterialsDockSession::observeProblems(MaterialProblems problems) {
    if (problems == problems_) return;
    const bool moved = problems.signature != problems_.signature;
    problems_ = std::move(problems);
    // Decision 3: the watch read the file Show the document saved -- taken
    // in (the problem went), or refused again for another reason.
    if (repair_ && repair_->watch && repair_->state == Repair::State::Waiting) {
        if (const auto *stale = staleOf(repair_->path)) {
            // EB-6s, decision 6 (engine EB-6): the refusal names the bytes it
            // read -- the ones saved means this save was refused, even for the
            // same reason at the same line; other bytes are the save before
            // it, still waiting. An engine before EB-6 names none: the
            // sentence moving is all there is to go by.
            const auto line = materialFileProblemLine(*stale);
            const bool this_save = !stale->digest.empty() && !repair_->saved_digest.empty()
                                       ? stale->digest == repair_->saved_digest
                                       : line != repair_->stale_line;
            if (this_save) {
                repair_->state = Repair::State::Refused;
                repair_->sentence = repair_->path + ": " + line;
                setNotice(repair_->sentence);
            }
        } else {
            loadedAgain(repair_->path, {});
        }
    }
    // Decision 3: the document shown as kept from before reads again, and the
    // list holds it loaded: its first material is selected.
    if (broken_ && !staleOf(*broken_))
        if (const auto *listed = documentOf(*broken_);
            listed != nullptr && listed->status == "loaded" && !listed->materials.empty()) {
            const Selection first{*broken_, listed->materials.front().name};
            broken_.reset();
            select(first);
        }
    // Decision 2: the engine took a document in, or refused one, on its own
    // (its watch): the list is read again.
    if (moved && ready_) {
        if (list_request_ != 0) list_request_ = 0;
        readList();
    }
    notify();
}

bool MaterialsDockSession::remove(const std::string &name) {
    const auto *document = materialDocumentOf(list_, name);
    if (name.empty() || document == nullptr) return false;
    Write write;
    write.kind = Write::Kind::Delete;
    write.method = "delete_material";
    write.path = document->path;
    write.name = name;
    write.description = "Delete " + name;
    write.params = Json{{"name", name}};
    return enqueue(std::move(write));
}

bool MaterialsDockSession::rename(const std::string &name, const std::string &new_name) {
    const auto *document = materialDocumentOf(list_, name);
    if (document == nullptr || name == new_name) return false;
    if (!validMaterialName(new_name)) {
        say("A material name is letters, digits and _.");
        return false;
    }
    Write write;
    write.kind = Write::Kind::Rename;
    write.method = "rename_material";
    write.path = document->path;
    write.name = new_name;
    write.description = "Rename " + name + " to " + new_name;
    write.params = Json{{"name", name}, {"new_name", new_name}};
    return enqueue(std::move(write));
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
    // MR-1s: register_material said listed already for a document that was
    // not loaded -- the engine's watch took it in first, or the engine holds
    // it as it read it before the file broke.
    const bool rechecked = recheck_.has_value();
    if (recheck_) {
        const auto path = std::exchange(recheck_, std::nullopt).value();
        const auto after = std::ranges::find(list.documents, path, &MaterialListDocument::path);
        if (after != list.documents.end() && after->status == "loaded" && !staleOf(path)) {
            std::vector<std::string> names;
            for (const auto &material : after->materials) names.push_back(material.name);
            loadedAgain(path, std::move(names));
        } else {
            const auto own = "The engine still holds " + path +
                             " as it read it before; a restart reads the file again (material.path_conflict@1).";
            restart_reason_ = "the engine reads " + path + " again when it starts again";
            setNotice(own);
            if (repair_ && repair_->path == path && repair_->state == Repair::State::Waiting) {
                repair_->state = Repair::State::Refused;
                repair_->sentence = own;
            }
        }
    }
    // EB-6s, decision 1: a document written that the engine cannot draw yet
    // -- its row, with why and Open the pass; already taken in (the pass
    // declared it meanwhile), its first material.
    if (broken_when_listed_) {
        const auto path = *broken_when_listed_;
        const auto after = std::ranges::find(list.documents, path, &MaterialListDocument::path);
        if (after != list.documents.end() && after->status == "unusable") {
            broken_when_listed_.reset();
            select_when_listed_.reset();
            list_ = std::move(list);
            if (listChanged) listChanged();
            selectBroken(path);
            notify();
            return;
        }
        if (after != list.documents.end() && after->status == "loaded") {
            broken_when_listed_.reset();
            if (!after->materials.empty()) select_when_listed_ = Selection{path, after->materials.front().name};
        }
    }
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
    if (!first && list == list_) {
        if (rechecked) notify();
        return;
    }
    // FO-1s, decision 2: the document shown as not loaded is loaded now (a
    // restart, or Register again): its first material is selected; gone from
    // the list, nothing is. MR-1s, decision 3: not while the engine's watch
    // still refuses its file.
    if (broken_ && !staleOf(*broken_)) {
        const auto after = std::ranges::find(list.documents, *broken_, &MaterialListDocument::path);
        if (after == list.documents.end() || (after->status == "loaded" && !after->materials.empty())) {
            const auto path = *broken_;
            const auto name = after == list.documents.end() ? std::string{} : after->materials.front().name;
            broken_.reset();
            list_ = std::move(list);
            if (listChanged) listChanged();
            if (!name.empty()) select({path, name});
            notify();
            return;
        }
    }
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
    // FO-1s, decisions 2 and 3.
    case Write::Kind::Register:
    case Write::Kind::UndoDelete: {
        const auto path = text(result, "path").empty() ? write.path : text(result, "path");
        const auto names = strings(result.value("names", Json::array()));
        if (write.kind == Write::Kind::UndoDelete) {
            if (!undo_.empty()) undo_.pop_back();
            setNotice("Put " + (names.empty() ? write.name : names.front()) + " back from " + path + ".");
        } else if (result.value("recovered", false)) {
            // MR-1s, decisions 1 and 2 (engine EB-2): a document the engine
            // refused at its start, taken in -- the problem list too.
            loadedAgain(write.path, names);
        } else if (result.value("reloaded", false)) {
            // EB-6s, decision 3 (engine EB-6): a registered document read
            // again from its file -- what Show the document saved, or Reload
            // from file after another editor. Its watch's refusal goes too.
            if (write.from_file && !(repair_ && repair_->path == path)) {
                std::size_t models = 0;
                if (const auto rebuilt = result.find("rebuilt_models"); rebuilt != result.end() && rebuilt->is_array())
                    models = rebuilt->size();
                setNotice("Read " + path + " again from its file; rebuilt " + std::to_string(models) +
                          (models == 1 ? " model." : " models."));
                restart_reason_.clear();
                if (problemsMayHaveChanged) problemsMayHaveChanged();
            } else {
                loadedAgain(path, names);
            }
            // What the editor shows is the file's now.
            if (selection_ && selection_->path == path) readSelected();
        } else {
            std::string listed;
            for (const auto &name : names) listed += (listed.empty() ? "" : ", ") + name;
            setNotice("Registered " + (listed.empty() ? path : listed + " from " + path) + ".");
        }
        if (!names.empty()) select_when_listed_ = Selection{path, names.front()};
        if (const auto digest = text(result, "material_digest"); !digest.empty()) digests_[path] = digest;
        if (broken_ && *broken_ == path) broken_.reset();
        break;
    }
    case Write::Kind::Delete: {
        const auto removed = result.value("removed", Json::object());
        const auto path = text(removed, "path").empty() ? write.path : text(removed, "path");
        UndoEntry entry;
        entry.kind = UndoEntry::Kind::Delete;
        entry.description = write.description;
        entry.path = path;
        entry.name = write.name;
        undo_.push_back(std::move(entry));
        setNotice("Deleted " + write.name + "; " + path + " stays on disk.");
        if (selection_ && selection_->name == write.name) clearSelection();
        break;
    }
    case Write::Kind::Rename:
    case Write::Kind::UndoRename: {
        const auto path = text(result, "path").empty() ? write.path : text(result, "path");
        const auto name = text(result, "name").empty() ? write.name : text(result, "name");
        const auto previous = text(result, "previous_name");
        if (write.kind == Write::Kind::UndoRename) {
            if (!undo_.empty()) undo_.pop_back();
        } else if (result.value("changed", true)) {
            UndoEntry entry;
            entry.kind = UndoEntry::Kind::Rename;
            entry.description = write.description;
            entry.path = path;
            entry.name = name;
            entry.previous = previous.empty() ? Json(write.params.value("name", std::string{})) : Json(previous);
            undo_.push_back(std::move(entry));
        }
        setNotice("Renamed " + (previous.empty() ? std::string{"it"} : previous) + " to " + name + ".");
        if (const auto digest = text(result, "material_digest"); !digest.empty()) digests_[path] = digest;
        select_when_listed_ = Selection{path, name};
        // The bindings that named it were rewritten with it.
        if (const auto rewritten = result.find("rewritten_bindings");
            rewritten != result.end() && rewritten->is_array())
            for (const auto &entry : *rewritten)
                if (const auto model = text(entry, "model"); !model.empty() && bound) bound(model);
        break;
    }
    }
    const auto restart = result.find("restart_required");
    if (text(result, "applied") == "unusable") {
        unusable(write, result);
    } else if (restart != result.end() && !restart->is_null()) {
        restart_reason_ = restart->is_string() ? restart->get<std::string>() : text(*restart, "reason");
        if (restart_reason_.empty()) restart_reason_ = "the engine reads it when it starts";
        if (restartRequested) restartRequested(restart_reason_);
    } else if (write.kind == Write::Kind::Edit || write.kind == Write::Kind::UndoEdit) {
        // MT-3s, decision 5 (engine MT-3): textures, tags, routing, variants
        // and surface are registered again in place, and the models drawing
        // the material rebuilt -- no restart.
        if (text(result, "applied") == "rebuilt") {
            std::size_t models = 0;
            if (const auto rebuilt = result.find("rebuilt_models"); rebuilt != result.end() && rebuilt->is_array())
                models = rebuilt->size();
            setNotice("Applied; rebuilt " + std::to_string(models) + (models == 1 ? " model." : " models."));
        }
        // A routing edit rewrote the bindings that name the material (and its
        // Undo writes them back): the rows that show them read them again.
        if (const auto rewritten = result.find("rewritten_bindings");
            rewritten != result.end() && rewritten->is_array())
            for (const auto &entry : *rewritten)
                if (const auto model = text(entry, "model"); !model.empty() && bound) bound(model);
    }
    readList();
}

void MaterialsDockSession::unusable(const Write &write, const Json &result) {
    // EB-6s, decision 1 (engine EB-6): written, and the engine goes on; the
    // pass that draws the material does not declare what it uses, so the
    // document is unusable until the pass does -- no restart.
    const auto problem = result.value("problem", Json::object());
    setNotice(materialUnusableSentence(problem), text(problem, "message"));
    restart_reason_.clear();
    auto path = text(problem, "material_document");
    if (path.empty()) path = text(result, "path");
    if (path.empty()) path = write.path;
    // The document's row (why, Open the pass) once the list says so. A bind
    // stays where it was made: the bound name draws once the pass declares it.
    if (write.kind != Write::Kind::Bind && write.kind != Write::Kind::UndoBind && !path.empty()) {
        select_when_listed_.reset();
        broken_when_listed_ = path;
    }
    if (repair_ && repair_->path == path && repair_->state == Repair::State::Waiting) {
        repair_->state = Repair::State::Refused;
        repair_->sentence = notice_;
    }
    // The engine lists it among its problems (source edit_rpc).
    if (problemsMayHaveChanged) problemsMayHaveChanged();
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
        // FO-1s, decisions 2 and 3: a delete or a rename says where the
        // material is in use; a document the engine refused at its start is
        // taken in only by a restart.
        const bool deleting = write.kind == Write::Kind::Delete || write.kind == Write::Kind::Rename ||
                              write.kind == Write::Kind::UndoRename;
        auto own = deleting ? materialDeleteRefusalDescription(error_code, error_fields,
                                                               write.kind != Write::Kind::Delete)
                            : std::string{};
        if (deleting && error_code == "material.in_use@1") {
            Uses uses;
            for (const auto &model : strings(error_fields.value("bindings", Json::array())))
                uses.models.push_back(model);
            uses.objects = strings(error_fields.value("objects", Json::array()));
            uses.count = std::max<std::size_t>(counter(error_fields, "count"), uses.objects.size());
            uses.documents = strings(error_fields.value("render_documents", Json::array()));
            uses_ = std::move(uses);
        }
        const auto *listed = documentOf(write.path);
        const bool registering = write.kind == Write::Kind::Register || write.kind == Write::Kind::UndoDelete;
        // MR-1s: listed already, for a document the list did not hold loaded
        // -- decided on the next list_materials (listResult), read below.
        const bool rechecking = registering && error_code == "material.path_conflict@1" && listed != nullptr &&
                                listed->status != "loaded";
        if (rechecking) {
            recheck_ = write.path;
            list_request_ = 0;
        } else if (registering) {
            // MR-1s, decision 1: the parser's refusal at its line and column;
            // Show the document stays open with it.
            own = materialRegisterRefusalLine(error_code, message, error_fields);
            if (repair_ && repair_->path == write.path && repair_->state == Repair::State::Waiting &&
                !repair_->watch) {
                repair_->state = Repair::State::Refused;
                repair_->sentence = own;
            }
        }
        if (error_code == "material.value_invalid@1") {
            field_error_param_ = error_fields.is_object() ? text(error_fields, "param") : std::string{};
            if (field_error_param_.empty()) field_error_param_ = write.param;
            field_error_ = sentence;
        } else if (!rechecking) {
            setNotice(own.empty() ? sentence : own, message);
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
        if (repair_ && repair_->state == Repair::State::Waiting && !repair_->watch) {
            repair_->state = Repair::State::Refused;
            repair_->sentence = notice_;
        }
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
    bindings_request_ = materials_request_ = object_request_ = bind_request_ = 0;
    object_edit_pending_ = false;
    object_requested_.reset();
    readAll();
    notify();
}

void MaterialAssignSession::engineGone() {
    ready_ = false;
    bindings_request_ = materials_request_ = object_request_ = bind_request_ = 0;
    object_edit_pending_ = false;
    object_requested_.reset();
    notify();
}

void MaterialAssignSession::setSubject(const Subject &subject) {
    if (subject == subject_) return;
    const bool other_model = subject.model != subject_.model;
    const bool other_object =
        other_model || subject.object != subject_.object || subject.scene_id != subject_.scene_id;
    const bool moved = subject.revision != subject_.revision;
    subject_ = subject;
    if (other_model) {
        primitives_.clear();
        bindings_read_ = false;
        counts_ = {};
        bindings_request_ = 0;
    }
    if (other_object) {
        object_materials_ = {};
        object_read_ = false;
        object_request_ = 0;
        object_edit_pending_ = false;
        object_requested_.reset();
        notice_.clear();
        notice_detail_.clear();
    }
    if (!subject_.model.empty()) {
        if (other_model) readMaterials();
        // N and M move with the scene (an object added, an override set), so
        // a new revision reads them again, as it reads the object's rows.
        if (other_model || moved) readBindings();
        if (other_object || moved) readObject();
    }
    notify();
}

void MaterialAssignSession::setSceneBusy(bool busy) {
    if (busy == scene_busy_) return;
    scene_busy_ = busy;
    if (!busy && object_edit_pending_) {
        // The edit reached its end (committed, refused or lost): what the
        // object carries now is the engine's to say.
        object_edit_pending_ = false;
        object_requested_.reset();
        readObject();
        readBindings();
    }
    notify();
}

void MaterialAssignSession::materialsChanged() { readMaterials(); }

void MaterialAssignSession::bindingsChanged() {
    readBindings();
    readObject();
}

void MaterialAssignSession::setScope(Scope scope) {
    if (scope == scope_) return;
    scope_ = scope;
    notify();
}

void MaterialAssignSession::readAll() {
    if (!ready_ || subject_.model.empty()) return;
    readMaterials();
    readBindings();
    readObject();
}

void MaterialAssignSession::readBindings() {
    if (!ready_ || subject_.model.empty()) return;
    bindings_request_ = send("get_material_bindings", Json{{"model", subject_.model}});
}

void MaterialAssignSession::readMaterials() {
    if (!ready_ || subject_.model.empty()) return;
    materials_request_ = send("list_materials", Json::object());
}

void MaterialAssignSession::readObject() {
    if (!ready_ || subject_.model.empty() || subject_.object == 0) return;
    Json params{{"object", subject_.object}};
    if (!subject_.scene_id.empty()) params["scene_id"] = subject_.scene_id;
    object_request_ = send("get_object_materials", params);
}

const std::string &MaterialAssignSession::label() const noexcept {
    return subject_.name.empty() ? subject_.model : subject_.name;
}

bool MaterialAssignSession::choosable() const noexcept {
    if (!ready_ || subject_.model.empty() || bind_request_ != 0) return false;
    if (scope_ == Scope::Model) return bindings_read_;
    return object_read_ && !scene_busy_ && !object_edit_pending_ && subject_.object != 0;
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

std::optional<std::string> MaterialAssignSession::shownObjectLabel() const {
    if (object_edit_pending_ && object_requested_) return *object_requested_ + " (this object)";
    const auto &primitives = object_materials_.primitives;
    if (primitives.empty()) return std::nullopt;
    const auto first = objectMaterialLabel(primitives.front());
    if (std::ranges::all_of(primitives, [&](const auto &p) { return objectMaterialLabel(p) == first; }))
        return first;
    return std::nullopt;
}

bool MaterialAssignSession::bind(Json params, std::string description, bool undoing,
                                 std::optional<std::string> requested) {
    if (!ready_ || bind_request_ != 0) return false;
    notice_.clear();
    notice_detail_.clear();
    bind_description_ = std::move(description);
    bind_model_ = params.value("model", std::string{});
    bind_undoing_ = undoing;
    requested_ = std::move(requested);
    bind_request_ = send("bind_material", params);
    notify();
    return bind_request_ != 0;
}

bool MaterialAssignSession::editObject(Json params, std::string description,
                                       std::optional<std::string> requested) {
    if (!choosable() || scope_ != Scope::Object || !transport_.edit_object) return false;
    params["object"] = subject_.object;
    if (!subject_.scene_id.empty()) params["scene_id"] = subject_.scene_id;
    notice_.clear();
    notice_detail_.clear();
    std::string error;
    if (!transport_.edit_object(params, description, &error)) {
        notice_ = error.empty() ? std::string{"The scene is busy with another edit; try again."} : error;
        notice_detail_ = error;
        notify();
        return false;
    }
    object_edit_pending_ = true;
    object_requested_ = std::move(requested);
    notify();
    return true;
}

bool MaterialAssignSession::choose(const std::string &material) {
    if (scope_ == Scope::Object) {
        if (!object_read_) return false;
        if (material.empty()) return useModelDefault();
        // Every primitive: the all-primitive row, and a primitive's own row
        // taken away so it does not keep beating it -- one edit.
        auto entries = Json::array({{{"material", material}}});
        bool same = false;
        for (const auto &row : object_materials_.rows) {
            if (!row.primitive) {
                same = row.material == material;
                continue;
            }
            entries.push_back({{"material", nullptr},
                               {"primitive", {{"mesh", row.primitive->first}, {"primitive", row.primitive->second}}}});
        }
        if (same && entries.size() == 1) return false;
        return editObject(Json{{"materials", entries}}, material + " on " + label(), material);
    }
    if (!bindings_read_) return false;
    const auto every = "every " + modelShortName(subject_.model);
    if (!material.empty())
        return bind(Json{{"model", subject_.model}, {"material", material}}, material + " on " + every, false,
                    material);
    auto bindings = Json::array();
    for (const auto &primitive : primitives_)
        if (primitive.material != primitive.slot)
            bindings.push_back(
                {{"mesh", primitive.mesh}, {"primitive", primitive.primitive}, {"material", primitive.slot}});
    if (bindings.empty()) return false;
    return bind(Json{{"model", subject_.model}, {"bindings", bindings}}, "Model default on " + every, false,
                std::string{});
}

bool MaterialAssignSession::choosePrimitive(std::size_t index, const std::string &material) {
    if (scope_ == Scope::Object) {
        if (index >= object_materials_.primitives.size()) return false;
        const auto &primitive = object_materials_.primitives[index];
        const Json target{{"mesh", primitive.mesh}, {"primitive", primitive.primitive}};
        const auto where = " on mesh " + std::to_string(primitive.mesh) + " primitive " +
                           std::to_string(primitive.primitive) + " of " + label();
        if (material.empty()) {
            const auto key = std::pair{primitive.mesh, primitive.primitive};
            const bool own = std::ranges::any_of(object_materials_.rows,
                                                 [&](const auto &row) { return row.primitive == key; });
            const bool all = std::ranges::any_of(object_materials_.rows,
                                                 [](const auto &row) { return !row.primitive; });
            // Its own row taken away; under an all-primitive row that would
            // still beat the model, the model's material is named instead.
            if (own) return editObject(Json{{"material", nullptr}, {"primitive", target}}, "Model default" + where,
                                       std::nullopt);
            if (all && !primitive.model_material.empty())
                return editObject(Json{{"material", primitive.model_material}, {"primitive", target}},
                                  "Model default" + where, std::nullopt);
            return false;
        }
        if (primitive.source == "object" && primitive.material == material) return false;
        return editObject(Json{{"material", material}, {"primitive", target}}, material + where, std::nullopt);
    }
    if (index >= primitives_.size()) return false;
    const auto &primitive = primitives_[index];
    const auto chosen = material.empty() ? primitive.slot : material;
    if (chosen == primitive.material) return false;
    return bind(Json{{"model", subject_.model},
                     {"bindings", Json::array({{{"mesh", primitive.mesh},
                                                {"primitive", primitive.primitive},
                                                {"material", chosen}}})}},
                (material.empty() ? std::string{"Model default"} : material) + " on mesh " +
                    std::to_string(primitive.mesh) + " primitive " + std::to_string(primitive.primitive) +
                    " of every " + modelShortName(subject_.model),
                false);
}

bool MaterialAssignSession::canUseModelDefault() const noexcept {
    return scope_ == Scope::Object && choosable() && !object_materials_.rows.empty();
}

bool MaterialAssignSession::useModelDefault() {
    if (!canUseModelDefault()) return false;
    auto entries = Json::array();
    for (const auto &row : object_materials_.rows) {
        Json entry{{"material", nullptr}};
        if (row.primitive) entry["primitive"] = {{"mesh", row.primitive->first}, {"primitive", row.primitive->second}};
        entries.push_back(std::move(entry));
    }
    return editObject(Json{{"materials", entries}}, "Model default on " + label(), std::nullopt);
}

bool MaterialAssignSession::undo() {
    if (undo_.empty()) return false;
    // To the model the bind went to, not the one selected now.
    const auto &entry = undo_.back();
    return bind(Json{{"model", entry.model}, {"bindings", entry.bindings}}, entry.description, true);
}

void MaterialAssignSession::objectEditRefused(const std::string &error_code, const std::string &message,
                                              const Json &error_fields) {
    object_edit_pending_ = false;
    object_requested_.reset();
    notice_ = sentenceFor(error_code, message, error_fields);
    notice_detail_ = message;
    readObject();
    notify();
}

bool MaterialAssignSession::owns(std::int64_t id) const noexcept {
    return id != 0 &&
           (id == bindings_request_ || id == materials_request_ || id == object_request_ || id == bind_request_);
}

bool MaterialAssignSession::receiveResult(std::int64_t id, const Json &result) {
    if (!owns(id)) return false;
    if (id == bindings_request_) {
        bindings_request_ = 0;
        primitives_ = parseMaterialBindings(result);
        counts_ = parseModelObjectCounts(result);
        bindings_read_ = true;
    } else if (id == materials_request_) {
        materials_request_ = 0;
        materials_ = materialNames(parseMaterialList(result));
    } else if (id == object_request_) {
        object_request_ = 0;
        object_materials_ = parseObjectMaterials(result);
        object_read_ = true;
    } else if (id == bind_request_) {
        bind_request_ = 0;
        if (bind_undoing_) {
            if (!undo_.empty()) undo_.pop_back();
        } else {
            const auto previous = result.value("previous", Json::object()).value("bindings", Json::array());
            if (result.value("changed", true) && previous.is_array() && !previous.empty())
                undo_.push_back({bind_description_, bind_model_, previous});
        }
        const auto restart = result.find("restart_required");
        if (text(result, "applied") == "unusable") {
            // EB-6s, decision 1 (engine EB-6): bound, and the model drawn with
            // its own material until the pass declares what the bound one
            // uses -- no restart.
            const auto problem = result.value("problem", Json::object());
            notice_ = materialUnusableSentence(problem);
            notice_detail_ = text(problem, "message");
        } else if (restart != result.end() && !restart->is_null()) {
            auto reason = restart->is_string() ? restart->get<std::string>() : text(*restart, "reason");
            if (reason.empty()) reason = "the engine reads it when it starts";
            notice_ = "Restart the engine to apply: " + reason;
            notice_detail_ = reason;
            if (restartRequested) restartRequested(reason);
        }
        readBindings();
        readObject();
    }
    notify();
    return true;
}

bool MaterialAssignSession::receiveRefusal(std::int64_t id, int code, const std::string &error_code,
                                           const std::string &message, const Json &error_fields) {
    if (!owns(id)) return false;
    if (id == object_request_ && code == -32601) {
        // An engine from before MT-2: no object of its can carry its own.
        object_request_ = 0;
        notice_ = "This engine cannot give an object its own material (no get_object_materials); "
                  "choose Every " + modelShortName(subject_.model) + ".";
        notice_detail_ = message;
        notify();
        return true;
    }
    if (error_code.empty()) return receiveFailure(id, message);
    if (id == bindings_request_) bindings_request_ = 0;
    else if (id == materials_request_) materials_request_ = 0;
    else if (id == object_request_) object_request_ = 0;
    else if (id == bind_request_) {
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
    } else if (id == object_request_) {
        object_request_ = 0;
        notice_ = "Could not read the object's materials: " + message;
        notice_detail_ = message;
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
