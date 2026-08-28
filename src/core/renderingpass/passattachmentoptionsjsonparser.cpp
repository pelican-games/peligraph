#include "passattachmentoptionsjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace Pelican {

namespace {

bool hasAttachment(const nlohmann::json &encoded) {
    if (encoded.is_null()) return false;
    if (encoded.is_string()) {
        return !encoded.get_ref<const std::string &>().empty();
    }
    if (encoded.is_object()) {
        const auto target = encoded.find("target");
        return target != encoded.end() && target->is_string() &&
               !target->get_ref<const std::string &>().empty();
    }
    if (encoded.is_array()) {
        return std::any_of(
            encoded.begin(), encoded.end(),
            [](const auto &entry) { return hasAttachment(entry); });
    }
    return false;
}

} // namespace

void validatePassAttachmentOperationsHaveOutputs(
    const nlohmann::json &pass_json, std::string_view pass_name) {
    const auto output = pass_json.find("output");
    const auto has_output = [&](std::string_view aspect) {
        if (output == pass_json.end() || !output->is_object()) return false;
        const auto found = output->find(std::string{aspect});
        return found != output->end() && hasAttachment(*found);
    };
    const auto require_output = [&](std::string_view field,
                                    std::string_view aspect) {
        if (pass_json.contains(field) && !has_output(aspect)) {
            throw std::runtime_error(
                "Pass '" + std::string{pass_name} + "' field '" +
                std::string{field} + "' requires a non-empty output." +
                std::string{aspect} + " attachment");
        }
    };
    require_output("color_load_op", "color");
    require_output("color_store_op", "color");
    require_output("depth_load_op", "depth");
    require_output("depth_store_op", "depth");
}

void parsePassAttachmentOptionsFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (pass_json.contains("clear_color")) {
        pass_def.clear_color = jsonToClearColor(pass_json.at("clear_color"));
    }
    pass_def.color_clear_values.clear();
    if (pass_json.contains("clear_colors")) {
        const auto &encoded =
            pass_json.at("clear_colors");
        if (!encoded.is_object()) {
            throw std::runtime_error(
                "clear_colors must be an object keyed by color "
                "output name: " +
                pass_def.name);
        }
        if (!pass_json.contains("output") ||
            !pass_json.at("output").is_object() ||
            !pass_json.at("output").contains("color")) {
            throw std::runtime_error(
                "clear_colors requires pass output.color: " +
                pass_def.name);
        }
        auto output_names =
            pass_json.at("output").at("color");
        if (output_names.is_null()) {
            output_names =
                nlohmann::json::array();
        } else if (!output_names.is_array()) {
            auto output_name = std::move(output_names);
            output_names =
                nlohmann::json::array(
                    {std::move(output_name)});
        }
        if (output_names.size() !=
            pass_def.output_color.size()) {
            throw std::runtime_error(
                "clear_colors output metadata is inconsistent: " +
                pass_def.name);
        }
        pass_def.color_clear_values.assign(
            pass_def.output_color.size(),
            pass_def.clear_color);
        for (auto field = encoded.begin();
             field != encoded.end(); ++field) {
            const auto found = std::find_if(
                output_names.begin(),
                output_names.end(),
                [&](const auto &name) {
                    if (name.is_string()) {
                        return name.get_ref<
                                   const std::string &>() ==
                               field.key();
                    }
                    return name.is_object() &&
                           name.contains("target") &&
                           name.at("target").is_string() &&
                           name.at("target")
                                   .get_ref<const std::string &>() ==
                               field.key();
                });
            if (found == output_names.end()) {
                throw std::runtime_error(
                    "clear_colors names a non-output render target '" +
                    field.key() + "': " +
                    pass_def.name);
            }
            const auto index =
                static_cast<std::size_t>(
                    std::distance(
                        output_names.begin(), found));
            pass_def.color_clear_values[index] =
                jsonToClearColor(field.value());
        }
    }
    if (pass_json.contains("color_load_op")) {
        pass_def.color_load_op =
            stringToLoadOp(parseStringField(pass_json, "color_load_op", "pass: " + pass_def.name));
    }
    if (pass_json.contains("color_store_op")) {
        pass_def.color_store_op =
            stringToStoreOp(parseStringField(pass_json, "color_store_op", "pass: " + pass_def.name));
    }
    if (pass_json.contains("depth_load_op")) {
        pass_def.depth_load_op =
            stringToLoadOp(parseStringField(pass_json, "depth_load_op", "pass: " + pass_def.name));
    }
    if (pass_json.contains("depth_store_op")) {
        pass_def.depth_store_op =
            stringToStoreOp(parseStringField(pass_json, "depth_store_op", "pass: " + pass_def.name));
    }
}

} // namespace Pelican
