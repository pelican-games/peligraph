#include "shadowdepthpasscontainer.hpp"

#include <limits>
#include <stdexcept>

namespace Pelican {

namespace {

PassId pipelineIndexToPassId(size_t pipeline_index) {
    if (pipeline_index > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Shadow depth pipeline id is too large");
    }
    return PassId{static_cast<int>(pipeline_index)};
}

PipelineHandle requirePipeline(PassId pass_id,
                               const std::unordered_map<int, PipelineHandle> &pipelines) {
    const auto found = pipelines.find(pass_id.value);
    if (found == pipelines.end()) {
        throw std::runtime_error("Shadow depth pipeline not found");
    }
    return found->second;
}

} // namespace

PassId ShadowDepthPassContainer::registerShadowDepthPass(vk::Format depth_format,
                                                         ShaderBundleId vert_shader,
                                                         std::vector<std::string> shader_defines) {
    const auto pass_id = pipelineIndexToPassId(pipelines.size());

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

    pipelines.emplace(pass_id.value, GET_MODULE(PipelineFactory).create(desc));
    return pass_id;
}

void ShadowDepthPassContainer::bind(vk::CommandBuffer cmd_buf, PassId pass_id) const {
    const auto pipeline = requirePipeline(pass_id, pipelines);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(pipeline));
}

vk::PipelineLayout ShadowDepthPassContainer::pipelineLayout(PassId pass_id) const {
    return GET_MODULE(PipelineFactory).layout(requirePipeline(pass_id, pipelines));
}

} // namespace Pelican
