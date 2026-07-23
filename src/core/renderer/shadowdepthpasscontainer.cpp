#include "shadowdepthpasscontainer.hpp"
#include "../shader/shaderlibrary.hpp"
#include "battery/embed.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Pelican {

namespace {

const ShadowDepthPassContainer::PipelineVariants &requirePipeline(
    PassId pass_id, const std::unordered_map<int, ShadowDepthPassContainer::PipelineVariants> &pipelines) {
    const auto found = pipelines.find(pass_id.value);
    if (found == pipelines.end()) {
        throw std::runtime_error("Shadow depth pipeline not found");
    }
    return found->second;
}

} // namespace

PassId ShadowDepthPassContainer::registerShadowDepthPass(vk::Format depth_format,
                                                         ShaderBundleId vert_shader,
                                                         std::vector<std::string> shader_defines,
                                                         vk::SampleCountFlagBits samples) {
    registration_order.reserve(registration_order.size() + 1);
    if (next_pass_id == std::numeric_limits<int>::max()) {
        throw std::runtime_error(
            "Shadow depth pipeline id table is exhausted");
    }
    const auto pass_id = PassId{next_pass_id++};

    GraphicsPipelineDesc desc;
    desc.vert = vert_shader;
    desc.frag = std::nullopt;
    desc.depth_format = depth_format;
    desc.shader_defines = std::move(shader_defines);
    desc.use_engine_vertex_layout = true;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_compare = vk::CompareOp::eLessOrEqual;
    desc.cull_mode = vk::CullModeFlagBits::eBack;
    desc.front_face = vk::FrontFace::eClockwise;
    desc.rasterization_samples = samples;

    const auto regular = GET_MODULE(PipelineFactory).create(desc);
    const auto embedded = b::embed<"skinned_shadow_depth.vert.spv">();
    desc.vert = GET_MODULE(ShaderLibrary).loadFromBytes(
        embedded.length(), embedded.data(), "skinned_shadow_depth.vert.spv");
    desc.use_engine_vertex_layout = false;
    desc.use_skinned_vertex_layout = true;
    const auto skinned = GET_MODULE(PipelineFactory).create(desc);
    if (!pipelines
             .emplace(pass_id.value,
                      PipelineVariants{regular, skinned})
             .second) {
        throw std::runtime_error(
            "Shadow depth pipeline table changed during registration");
    }
    registration_order.push_back(pass_id);
    return pass_id;
}

void ShadowDepthPassContainer::bind(vk::CommandBuffer cmd_buf, PassId pass_id, bool skinned) const {
    const auto &variants = requirePipeline(pass_id, pipelines);
    const auto pipeline = skinned ? variants.skinned : variants.regular;
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(pipeline));
}

vk::PipelineLayout ShadowDepthPassContainer::pipelineLayout(PassId pass_id, bool skinned) const {
    const auto &variants = requirePipeline(pass_id, pipelines);
    return GET_MODULE(PipelineFactory).layout(skinned ? variants.skinned : variants.regular);
}

ShadowDepthPassContainer::RegistrationCheckpoint
ShadowDepthPassContainer::checkpointRegistrations() const noexcept {
    return RegistrationCheckpoint{
        registration_order.size(), next_pass_id};
}

void ShadowDepthPassContainer::rollbackRegistrations(
    RegistrationCheckpoint checkpoint) {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Shadow depth pass registration checkpoint is invalid");
    }
    while (registration_order.size() >
           checkpoint.registration_count) {
        pipelines.erase(registration_order.back().value);
        registration_order.pop_back();
    }
    next_pass_id = checkpoint.next_pass_id;
}

std::vector<PassId>
ShadowDepthPassContainer::registrationsSince(
    RegistrationCheckpoint checkpoint) const {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Shadow depth pass registration checkpoint is invalid");
    }
    return {
        registration_order.begin() +
            static_cast<std::ptrdiff_t>(
                checkpoint.registration_count),
        registration_order.end()};
}

void ShadowDepthPassContainer::retireRegistrations(
    const std::vector<PassId> &ids) noexcept {
    for (const auto id : ids) {
        pipelines.erase(id.value);
        std::erase(registration_order, id);
    }
}

} // namespace Pelican
