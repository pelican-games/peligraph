#include "velocitypasscontainer.hpp"

#include <limits>
#include <stdexcept>

namespace Pelican {
namespace {

PassId pipelineIndexToPassId(size_t index) {
    if (index > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Velocity pipeline id is too large");
    }
    return PassId{static_cast<int>(index)};
}

const VelocityPassContainer::PipelineVariants &requirePipeline(
    PassId pass_id,
    const std::unordered_map<int, VelocityPassContainer::PipelineVariants> &pipelines) {
    const auto found = pipelines.find(pass_id.value);
    if (found == pipelines.end()) throw std::runtime_error("Velocity pipeline not found");
    return found->second;
}

} // namespace

PassId VelocityPassContainer::registerVelocityPass(
    vk::Format color_format, vk::Format depth_format, ShaderBundleId regular_vert,
    ShaderBundleId skinned_vert, ShaderBundleId frag,
    std::vector<std::string> shader_defines,
    vk::SampleCountFlagBits samples) {
    const auto pass_id = pipelineIndexToPassId(pipelines.size());
    GraphicsPipelineDesc desc;
    desc.vert = regular_vert;
    desc.frag = frag;
    desc.color_formats = {color_format};
    desc.depth_format = depth_format;
    desc.shader_defines = shader_defines;
    desc.use_engine_vertex_layout = true;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_compare = vk::CompareOp::eLessOrEqual;
    desc.cull_mode = vk::CullModeFlagBits::eBack;
    desc.front_face = vk::FrontFace::eClockwise;
    desc.rasterization_samples = samples;
    const auto regular = GET_MODULE(PipelineFactory).create(desc);

    desc.vert = skinned_vert;
    desc.use_engine_vertex_layout = false;
    desc.use_skinned_vertex_layout = true;
    const auto skinned = GET_MODULE(PipelineFactory).create(desc);
    pipelines.emplace(pass_id.value, PipelineVariants{regular, skinned});
    return pass_id;
}

void VelocityPassContainer::bind(vk::CommandBuffer cmd_buf, PassId pass_id,
                                 bool skinned) const {
    const auto &variants = requirePipeline(pass_id, pipelines);
    const auto pipeline = skinned ? variants.skinned : variants.regular;
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                         GET_MODULE(PipelineFactory).pipeline(pipeline));
}

vk::PipelineLayout VelocityPassContainer::pipelineLayout(PassId pass_id,
                                                          bool skinned) const {
    const auto &variants = requirePipeline(pass_id, pipelines);
    return GET_MODULE(PipelineFactory).layout(skinned ? variants.skinned : variants.regular);
}

} // namespace Pelican
