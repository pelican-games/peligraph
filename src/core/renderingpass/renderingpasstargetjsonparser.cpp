#include "renderingpasstargetjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetnameresolver.hpp"
#include "../../project/imagesubresourcejson.hpp"
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

struct InputReference {
    std::string name;
    bool history = false;
};

InputReference parseInputReference(const std::string &authored) {
    constexpr std::string_view suffix = "@history";
    if (authored.size() >= suffix.size() && authored.ends_with(suffix)) {
        const auto name = authored.substr(0, authored.size() - suffix.size());
        if (name.empty() || name.find('@') != std::string::npos) {
            throw std::runtime_error("Invalid history input target: " + authored);
        }
        return {name, true};
    }
    if (authored.find('@') != std::string::npos) {
        throw std::runtime_error("Unknown input target qualifier: " + authored);
    }
    return {authored, false};
}

GlobalRenderTargetId resolveRenderTarget(const RenderTargetNameResolver &rt_resolver, const std::string &name,
                                         const std::string &role) {
    const auto rt_id = rt_resolver.resolve(name);
    if (!isConcreteRenderTarget(rt_id)) {
        throw std::runtime_error(role + " render target not found: " + name);
    }
    return rt_id;
}

void validateRasterSubresource(
    const std::optional<ImageSubresourceRange> &subresource,
    std::string_view context) {
    if (!subresource) return;
    if (subresource->mip_count_mode !=
            ImageSubresourceMipCountMode::fixed ||
        subresource->level_count != 1) {
        throw std::runtime_error(
            std::string{context} +
            ".subresource must select exactly one mip level");
    }
}

RasterAttachmentView parseRasterAttachment(
    const RenderTargetNameResolver &rt_resolver,
    const nlohmann::json &encoded,
    std::string_view role,
    bool allow_swapchain) {
    std::string name;
    std::optional<ImageSubresourceRange> subresource;
    if (encoded.is_string()) {
        name = encoded.get<std::string>();
    } else if (encoded.is_object()) {
        for (auto field = encoded.begin();
             field != encoded.end(); ++field) {
            if (field.key() != "target" &&
                field.key() != "subresource") {
                throw std::runtime_error(
                    std::string{role} +
                    " output has unknown field '" +
                    field.key() + "'");
            }
        }
        if (!encoded.contains("target") ||
            !encoded.at("target").is_string()) {
            throw std::runtime_error(
                std::string{role} +
                " output object requires string field target");
        }
        name = encoded.at("target").get<std::string>();
        subresource = parseOptionalImageSubresource(
            encoded, std::string{role} + " output");
        validateRasterSubresource(
            subresource,
            std::string{role} + " output");
    } else {
        throw std::runtime_error(
            std::string{role} +
            " output must be a render target name or object");
    }

    validateName(name, std::string{role} + " output target");
    if (name == "swapchain") {
        if (!allow_swapchain) {
            throw std::runtime_error(
                std::string{role} +
                " output target cannot be swapchain");
        }
        if (subresource) {
            throw std::runtime_error(
                "Swapchain output cannot select a subresource");
        }
        return RasterAttachmentView{
            swapchainRenderTargetId()};
    }
    return RasterAttachmentView{
        resolveRenderTarget(
            rt_resolver, name,
            std::string{role}),
        std::move(subresource)};
}

} // namespace

void parsePassOutputTargetsFromJson(PassDefinition &pass_def, const RenderTargetNameResolver &rt_resolver,
                                    const nlohmann::json &pass_json) {
    if (!pass_json.contains("output")) {
        throw std::runtime_error("Pass requires output field: " + pass_def.name);
    }

    const auto &output = pass_json.at("output");
    if (!output.is_object()) {
        throw std::runtime_error("Pass output must be an object: " + pass_def.name);
    }
    if (!output.contains("color") || !output.contains("depth")) {
        throw std::runtime_error("Pass output requires color and depth fields: " + pass_def.name);
    }

    pass_def.output_color = parseColorOutputTargetsFromJson(rt_resolver, output.at("color"));
    pass_def.output_depth = parseDepthOutputTargetFromJson(rt_resolver, output.at("depth"));
}

void parsePassInputTargetsFromJson(PassDefinition &pass_def, const RenderTargetNameResolver &rt_resolver,
                                   const nlohmann::json &pass_json,
                                   const std::unordered_set<std::string> &buffer_names) {
    if (!pass_json.contains("input")) {
        return;
    }

    parseInputResourcesFromJson(pass_def, rt_resolver, pass_json.at("input"), buffer_names);
}

std::vector<RasterAttachmentView>
parseColorOutputTargetsFromJson(
    const RenderTargetNameResolver &rt_resolver,
    nlohmann::json color_output) {
    if (color_output.is_null()) {
        return {};
    }
    if (!color_output.is_array()) {
        color_output = nlohmann::json::array({color_output});
    }

    std::vector<RasterAttachmentView> output_color;
    output_color.reserve(color_output.size());
    for (const auto &encoded : color_output) {
        output_color.push_back(
            parseRasterAttachment(
                rt_resolver, encoded, "Color", true));
    }
    return output_color;
}

RasterAttachmentView parseDepthOutputTargetFromJson(
    const RenderTargetNameResolver &rt_resolver,
    const nlohmann::json &depth_output) {
    if (depth_output.is_null()) {
        return RasterAttachmentView{noRenderTargetId()};
    }
    return parseRasterAttachment(
        rt_resolver, depth_output, "Depth", false);
}

std::vector<GlobalRenderTargetId> parseInputTargetsFromJson(const RenderTargetNameResolver &rt_resolver,
                                                            nlohmann::json input_output) {
    if (input_output.is_null()) {
        return {};
    }
    if (!input_output.is_array()) {
        input_output = nlohmann::json::array({input_output});
    }

    std::vector<GlobalRenderTargetId> input_targets;
    for (const auto &input_name_json : input_output) {
        if (!input_name_json.is_string()) {
            throw std::runtime_error("Input target must be a render target name");
        }
        const auto input = parseInputReference(input_name_json.get<std::string>());
        validateName(input.name, "Input target");
        if (input.name == "swapchain") {
            throw std::runtime_error("Input target cannot be swapchain");
        }
        input_targets.push_back(resolveRenderTarget(rt_resolver, input.name, "Input"));
    }
    return input_targets;
}

void parseInputResourcesFromJson(PassDefinition &pass_def, const RenderTargetNameResolver &rt_resolver,
                                 nlohmann::json input_output,
                                 const std::unordered_set<std::string> &buffer_names) {
    if (input_output.is_null()) {
        return;
    }
    if (!input_output.is_array()) {
        input_output = nlohmann::json::array({input_output});
    }

    for (const auto &input_name_json : input_output) {
        if (!input_name_json.is_string()) {
            throw std::runtime_error("Input target must be a render target or buffer name");
        }
        const auto input = parseInputReference(input_name_json.get<std::string>());
        validateName(input.name, "Input target");
        if (input.name == "swapchain") {
            throw std::runtime_error("Input target cannot be swapchain");
        }
        if (buffer_names.find(input.name) != buffer_names.end()) {
            if (input.history) {
                throw std::runtime_error("@history is supported only for render targets: " + input.name);
            }
            pass_def.input_buffers.push_back(input.name);
            continue;
        }
        pass_def.input_targets.push_back(resolveRenderTarget(rt_resolver, input.name, "Input"));
        pass_def.input_target_history.push_back(input.history);
    }
}

} // namespace Pelican
