#include "renderingsamplecount.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>
#include <vulkan/vulkan.hpp>
#if __has_include(<vulkan/utility/vk_format_utils.h>)
#include <vulkan/utility/vk_format_utils.h>
#define PELICAN_HAS_VULKAN_FORMAT_UTILS 1
#endif

namespace Pelican {
namespace {

class DisjointSet {
    std::vector<std::size_t> parents_;

  public:
    explicit DisjointSet(std::size_t size) : parents_(size) {
        std::iota(parents_.begin(), parents_.end(), 0);
    }

    std::size_t find(std::size_t value) {
        if (parents_[value] != value) {
            parents_[value] = find(parents_[value]);
        }
        return parents_[value];
    }

    void join(std::size_t left, std::size_t right) {
        left = find(left);
        right = find(right);
        if (left != right) parents_[right] = left;
    }
};

bool isAttachmentTarget(const RenderTargetDefinition &definition) {
    return bool(definition.usage &
                (vk::ImageUsageFlagBits::eColorAttachment |
                 vk::ImageUsageFlagBits::eDepthStencilAttachment));
}

bool isGeometryPass(std::string_view type) {
    return type == "material" || type == "shadow_depth" ||
           type == "velocity";
}

void appendTargetNames(const nlohmann::json &value,
                       std::vector<std::string> &names) {
    if (value.is_null()) return;
    if (value.is_string()) {
        const auto name = value.get<std::string>();
        if (name != "swapchain") names.push_back(name);
        return;
    }
    if (!value.is_array()) {
        throw std::runtime_error(
            "render pass output target must be a string, array, or null");
    }
    for (const auto &entry : value) appendTargetNames(entry, names);
}

std::vector<std::string> passAttachments(const nlohmann::json &pass) {
    std::vector<std::string> names;
    const auto output = pass.find("output");
    if (output == pass.end()) return names;
    if (!output->is_object()) {
        throw std::runtime_error("render pass output must be an object");
    }
    if (output->contains("color")) {
        appendTargetNames(output->at("color"), names);
    }
    if (output->contains("depth")) {
        appendTargetNames(output->at("depth"), names);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::vector<std::uint32_t> defaultSingleSample(
    const RenderTargetDefinition &) {
    return {1};
}

} // namespace

RenderingSampleCountResolution resolveRenderingSampleCounts(
    const nlohmann::json &rendering_config,
    std::span<const RenderTargetDefinition> render_targets,
    const SampleCountPolicy &policy,
    const AttachmentSampleCapabilityQuery &query_capabilities) {
    if (!rendering_config.is_object()) {
        throw std::runtime_error(
            "rendering config must be an object for sample-count planning");
    }
    std::vector<std::size_t> attachment_indices;
    std::unordered_map<std::string, std::size_t> attachment_by_name;
    for (std::size_t i = 0; i < render_targets.size(); ++i) {
        if (!isAttachmentTarget(render_targets[i])) continue;
        if (!attachment_by_name.emplace(render_targets[i].name,
                                        attachment_indices.size())
                 .second) {
            throw std::runtime_error(
                "duplicate render target in sample-count planning: " +
                render_targets[i].name);
        }
        attachment_indices.push_back(i);
    }

    DisjointSet sets{attachment_indices.size()};
    std::vector<bool> geometry_member(attachment_indices.size(), false);
    if (rendering_config.contains("rendering_passes")) {
        const auto &pass_sets = rendering_config.at("rendering_passes");
        if (!pass_sets.is_array()) {
            throw std::runtime_error("rendering_passes must be an array");
        }
        for (const auto &pass_set : pass_sets) {
            if (!pass_set.is_object() || !pass_set.contains("passes") ||
                !pass_set.at("passes").is_array()) {
                throw std::runtime_error(
                    "rendering pass requires passes array");
            }
            for (const auto &pass : pass_set.at("passes")) {
                if (!pass.is_object()) {
                    throw std::runtime_error(
                        "rendering pass entries must be objects");
                }
                const auto attachments = passAttachments(pass);
                std::vector<std::size_t> members;
                for (const auto &name : attachments) {
                    const auto found = attachment_by_name.find(name);
                    if (found == attachment_by_name.end()) continue;
                    members.push_back(found->second);
                }
                for (std::size_t i = 1; i < members.size(); ++i) {
                    sets.join(members.front(), members[i]);
                }
                if (isGeometryPass(
                        pass.value("type", std::string{}))) {
                    for (const auto member : members) {
                        geometry_member[member] = true;
                    }
                }
            }
        }
    }

    std::set<std::size_t> explicitly_selected;
    for (const auto &name : policy.targets) {
        const auto found = attachment_by_name.find(name);
        if (found == attachment_by_name.end()) {
            throw std::runtime_error(
                "multisampling target is not an attachment render target: " +
                name);
        }
        explicitly_selected.insert(found->second);
    }

    std::map<std::size_t, std::vector<std::size_t>> components;
    for (std::size_t i = 0; i < attachment_indices.size(); ++i) {
        components[sets.find(i)].push_back(i);
    }

    std::vector<SampleCountGroupRequest> groups;
    groups.reserve(components.size());
    const auto &query = query_capabilities
                            ? query_capabilities
                            : AttachmentSampleCapabilityQuery{
                                  defaultSingleSample};
    for (const auto &[root, members] : components) {
        (void)root;
        bool selected_by_scope = policy.scope == SampleCountScope::all;
        bool selected_explicitly = false;
        std::vector<SampleCountResourceCapability> resources;
        resources.reserve(members.size());
        for (const auto member : members) {
            if (policy.scope == SampleCountScope::geometry &&
                geometry_member[member]) {
                selected_by_scope = true;
            }
            if (explicitly_selected.contains(member)) {
                selected_explicitly = true;
            }
            const auto &definition =
                render_targets[attachment_indices[member]];
            resources.push_back({
                definition.name,
                vk::to_string(definition.format),
                query(definition),
            });
        }
        std::sort(resources.begin(), resources.end(),
                  [](const auto &left, const auto &right) {
                      return left.resource < right.resource;
                  });
        groups.push_back(SampleCountGroupRequest{
            "attachments:" + resources.front().resource,
            (policy.scope != SampleCountScope::none &&
             selected_by_scope) ||
                selected_explicitly,
            std::move(resources),
        });
    }

    auto plan = resolveSampleCountPlan(policy.request, groups);
    RenderingSampleCountResolution result;
    result.plan = std::move(plan);
    result.assignments.reserve(render_targets.size());
    std::unordered_map<std::string, std::uint32_t> planned;
    for (const auto &resource : result.plan.resources) {
        planned.emplace(resource.resource, resource.samples);
    }
    for (const auto &target : render_targets) {
        const auto found = planned.find(target.name);
        result.assignments.push_back(
            {target.name, found == planned.end() ? 1u : found->second});
    }
    std::sort(result.assignments.begin(), result.assignments.end(),
              [](const auto &left, const auto &right) {
                  return left.resource < right.resource;
              });
    return result;
}

void applyRenderingSampleCounts(
    std::span<RenderTargetDefinition> render_targets,
    const RenderingSampleCountResolution &resolution) {
    std::unordered_map<std::string, std::uint32_t> assignments;
    for (const auto &assignment : resolution.assignments) {
        if (!assignments.emplace(assignment.resource, assignment.samples)
                 .second) {
            throw std::runtime_error(
                "duplicate resolved sample-count assignment: " +
                assignment.resource);
        }
    }
    for (auto &target : render_targets) {
        const auto found = assignments.find(target.name);
        if (found == assignments.end()) {
            throw std::runtime_error(
                "missing resolved sample-count assignment: " + target.name);
        }
        target.samples = found->second;
    }
}

vk::ResolveModeFlagBits colorAttachmentResolveMode(
    vk::Format format) {
#if defined(PELICAN_HAS_VULKAN_FORMAT_UTILS)
    const auto raw = static_cast<VkFormat>(format);
    const auto integer =
        vkuFormatIsSINT(raw) || vkuFormatIsUINT(raw);
#else
    // Vulkan's integer color format enumerants use the Uint/Sint suffix.
    // This fallback keeps minimal Vulkan-Headers distributions buildable.
    const auto name = vk::to_string(format);
    const auto integer =
        name.find("Uint") != std::string::npos ||
        name.find("Sint") != std::string::npos;
#endif
    return integer ? vk::ResolveModeFlagBits::eSampleZero
                   : vk::ResolveModeFlagBits::eAverage;
}

} // namespace Pelican
