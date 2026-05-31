#include "polygoninstancecontainer.hpp"
#include "../vkcore/core.hpp"
#include <algorithm>
#include <glm/ext/matrix_transform.hpp>
#include <limits>
#include <stdexcept>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace Pelican {

namespace {

constexpr size_t maxModelInstances = 1024;
constexpr size_t maxRenderCommands = 1024;

uint32_t checkedDrawCount(size_t count) {
    if (count > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Draw call count exceeds uint32_t range");
    }
    return static_cast<uint32_t>(count);
}

} // namespace

static BufferWrapper createIndirectBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(RenderCommand) * num,
                           vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

static BufferWrapper createModelInstanceDataBuf(VulkanManageCore &vkcore, size_t num) {
    return vkcore.allocBuf(sizeof(glm::mat4) * num,
                           vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                               vk::BufferUsageFlagBits::eTransferDst,
                           vma::MemoryUsage::eAutoPreferDevice,
                           vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

PolygonInstanceContainer::PolygonInstanceContainer()
    : indirect_buf{
          createIndirectBuf(GET_MODULE(VulkanManageCore), maxRenderCommands),
      },
      model_data_buffer{
          createModelInstanceDataBuf(GET_MODULE(VulkanManageCore), maxModelInstances),
      } {}

ModelInstanceId PolygonInstanceContainer::placeModelInstance(ModelTemplate &model) {
    if (model_instances_data.size() >= maxModelInstances) {
        throw std::runtime_error("Model instance capacity exceeded");
    }

    size_t primitive_count = 0;
    for (const auto &material : model.material_primitives) {
        primitive_count += material.primitives.size();
    }
    if (render_commands.size() > maxRenderCommands ||
        primitive_count > maxRenderCommands - render_commands.size()) {
        throw std::runtime_error("Render command capacity exceeded");
    }

    ModelInstanceId id{static_cast<uint32_t>(model_instances_data.size())};
    model_instances_data.push_back(glm::identity<glm::mat4>());

    for (const auto &material : model.material_primitives) {
        for (const auto &primitive : material.primitives) {
            RenderCommand instance{
                .command =
                    vk::DrawIndexedIndirectCommand{
                        primitive.index_count,
                        1,
                        primitive.index_offset,
                        primitive.vert_offset,
                        id.value,
                    },
                .material = material.material,
            };
            render_commands.push_back(instance);
        }
    }
    return id;
}
void PolygonInstanceContainer::removeModelInstance(ModelInstanceId id) {
    if (id.value >= model_instances_data.size()) {
        return;
    }

    const auto first_instance = id.value;
    std::erase_if(render_commands, [first_instance](const RenderCommand &command) {
        return command.command.firstInstance == first_instance;
    });
    model_instances_data[id.value] = glm::identity<glm::mat4>();
}

void PolygonInstanceContainer::triggerUpdate() {
    // clear previous frame
    draw_calls.clear();

    if (render_commands.empty())
        return;

    // prepare indirect buffer
    std::sort(render_commands.begin(), render_commands.end(),
              [](const RenderCommand &p, const RenderCommand &q) { return p.material.value < q.material.value; });
    GET_MODULE(VulkanManageCore)
        .writeBuf(indirect_buf, render_commands.data(), 0, sizeof(RenderCommand) * render_commands.size());

    // prepare drawindirect information
    DrawIndirectInfo draw_call{.stride = sizeof(RenderCommand)};
    size_t prev_offset_index;

    prev_offset_index = 0;
    draw_call.material = render_commands[0].material;
    draw_call.offset = prev_offset_index * sizeof(RenderCommand);
    for (size_t i = 1; i < render_commands.size(); i++) {
        if (render_commands[i].material.value != render_commands[i - 1].material.value) {
            draw_call.draw_count = checkedDrawCount(i - prev_offset_index);
            draw_calls.push_back(draw_call);

            prev_offset_index = i;
            draw_call.material = render_commands[i].material;
            draw_call.offset = prev_offset_index * sizeof(RenderCommand);
        }
    }
    draw_call.draw_count = checkedDrawCount(render_commands.size() - prev_offset_index);
    draw_calls.push_back(draw_call);

    GET_MODULE(VulkanManageCore)
        .writeBuf(model_data_buffer, model_instances_data.data(), 0, sizeof(glm::mat4) * model_instances_data.size());
}

void PolygonInstanceContainer::setTrs(ModelInstanceId id, glm::vec3 pos, glm::quat rotation, glm::vec3 scale) {
    if (id.value >= model_instances_data.size()) {
        throw std::runtime_error("Model instance not found");
    }

    model_instances_data[id.value] = glm::translate(glm::identity<glm::mat4>(), pos) * glm::toMat4(rotation) *
                                     glm::scale(glm::identity<glm::mat4>(), scale);
}

const BufferWrapper &PolygonInstanceContainer::getIndirectBuf() const { return indirect_buf; }
const BufferWrapper &PolygonInstanceContainer::getObjectBuf() const { return model_data_buffer; }
const std::vector<DrawIndirectInfo> &PolygonInstanceContainer::getDrawCalls() const { return draw_calls; }

} // namespace Pelican
