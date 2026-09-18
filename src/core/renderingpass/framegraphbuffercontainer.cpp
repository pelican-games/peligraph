#include "framegraphbuffercontainer.hpp"
#include "deferredresourceretire.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetcontainer.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/raygrouplayout.hpp"
#include "../shader/raytracingvalidation.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../renderer/frameresources.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/render_target_layout_tracker.hpp"
#include "../vkcore/util.hpp"
#include "../../project/renderresourcename.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {

FrameGraphResourceContainer::FrameGraphResourceContainer() = default;

FrameGraphResourceContainer::~FrameGraphResourceContainer() = default;

void FrameGraphResourceContainer::registerBuffers(const std::vector<FrameGraphBufferDefinition> &definitions) {
    auto &vkcore = GET_MODULE(VulkanManageCore);
    for (const auto &definition : definitions) {
        if (definition.name.empty()) {
            throw std::runtime_error("buffer name must not be empty");
        }
        if (definition.size == 0) {
            continue;
        }
        if (name_to_id.contains(definition.name)) {
            continue;
        }
        const auto maximum_range =
            static_cast<vk::DeviceSize>(
                vkcore.getPhysDevice()
                    .getProperties()
                    .limits.maxStorageBufferRange);
        if (definition.size > maximum_range) {
            throw std::runtime_error(
                "Frame graph buffer '" + definition.name +
                "' requires " +
                std::to_string(definition.size) +
                " bytes, exceeding device maxStorageBufferRange " +
                std::to_string(maximum_range));
        }
        registration_order.reserve(
            registration_order.size() + 1);
        auto usage =
            vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferSrc |
            vk::BufferUsageFlagBits::eTransferDst;
        if (definition.index_buffer) usage |= vk::BufferUsageFlagBits::eIndexBuffer;
        if (definition.command_layout) {
            usage |= vk::BufferUsageFlagBits::
                eIndirectBuffer;
        }
        if (definition.command_layout == FrameGraphBufferCommandLayout::ray_dispatch) {
            if (!vkcore.getRuntimeCapabilities().ray_tracing_indirect)
                throw std::runtime_error("pelican.ray_tracing.indirect_unavailable@1: device does not support indirect ray dispatch");
            usage |= vk::BufferUsageFlagBits::eShaderDeviceAddress;
        }
        auto buffer = vkcore.allocBuf(
            definition.size,
            usage,
            definition.host_source
                ? vma::MemoryUsage::eAuto
                : vma::MemoryUsage::eAutoPreferDevice,
            definition.host_source
                ? vma::AllocationCreateFlagBits::
                      eHostAccessSequentialWrite
                : vma::AllocationCreateFlags{});
        vkcore.getDebugUtils().nameBuffer(
            buffer.buffer.get(),
            ("frame_graph/buffer/" +
             definition.name)
                .c_str());
        BufferRecord record{definition, std::move(buffer)};
        if (definition.host_source == FrameGraphHostBufferSource::runtime_f32) {
            if (definition.command_layout || definition.size % sizeof(float) != 0)
                throw std::invalid_argument("runtime_f32 requires a float-aligned data buffer");
            record.runtime_values.resize(definition.size / sizeof(float), 0.0f);
            // The declared start value, if the document carries one. A
            // `render.scalar` write replaces it on the frame it publishes.
            std::copy(definition.initial_f32.begin(), definition.initial_f32.end(),
                      record.runtime_values.begin());
            record.runtime_values_dirty = true;
        }
        const auto id = buffers.reg(std::move(record));
        try {
            if (!name_to_id.emplace(definition.name, id).second) {
                throw std::runtime_error(
                    "Frame graph buffer name table changed during registration: " +
                    definition.name);
            }
        } catch (...) {
            (void)buffers.extract(id, false);
            throw;
        }
        registration_order.push_back(id);
    }
}

bool FrameGraphResourceContainer::hasBuffer(std::string_view name) const {
    return isValidFrameGraphBufferId(getBufferIdByName(name));
}

void FrameGraphResourceContainer::copyBufferState(
    const std::vector<std::pair<FrameGraphBufferId, FrameGraphBufferId>> &copies) {
    if (copies.empty()) return;
    for (const auto &[source, destination] : copies) {
        const auto &old = buffers.get(source);
        auto &next = buffers.get(destination);
        if (old.definition.size != next.definition.size ||
            old.definition.host_source != next.definition.host_source)
            throw std::logic_error("graph buffer state transfer requires unchanged declarations");
        next.runtime_values = old.runtime_values;
        next.runtime_values_dirty = old.runtime_values_dirty;
        next.host_population = old.host_population;
    }
    GET_MODULE(VulkanUtils).executeOneTimeCmd([&](vk::CommandBuffer command) {
        const vk::MemoryBarrier before{vk::AccessFlagBits::eMemoryWrite,
            vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eTransfer, {}, before, {}, {});
        for (const auto &[source, destination] : copies)
            command.copyBuffer(buffer(source).buffer.get(), buffer(destination).buffer.get(),
                vk::BufferCopy{0, 0, bufferSize(source)});
        const vk::MemoryBarrier after{vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eAllCommands, {}, after, {}, {});
    }, true);
}

bool FrameGraphResourceContainer::stageRuntimeValues(std::string_view name, std::span<const float> values) {
    return stageRuntimeValues(getBufferIdByName(name), values);
}

bool FrameGraphResourceContainer::stageRuntimeValues(FrameGraphBufferId id, std::span<const float> values) {
    if (!isValidFrameGraphBufferId(id) || !buffers.contains(id)) return false;
    auto &record = buffers.get(id);
    if (record.definition.host_source != FrameGraphHostBufferSource::runtime_f32)
        throw std::invalid_argument("runtime values require host_source runtime_f32");
    if (values.size_bytes() != record.definition.size)
        throw std::invalid_argument("runtime values must fill the declared float buffer");
    std::copy(values.begin(), values.end(), record.runtime_values.begin());
    record.runtime_values_dirty = true;
    return true;
}

void FrameGraphResourceContainer::uploadRuntimeValues() {
    for (const auto id : registration_order) {
        if (!buffers.contains(id)) continue;
        auto &record = buffers.get(id);
        if (!record.runtime_values_dirty) continue;
        writeHostBuffer(id, std::as_bytes(std::span{record.runtime_values}));
        record.runtime_values_dirty = false;
    }
}

bool FrameGraphResourceContainer::hasBuffer(
    FrameGraphBufferId id) const {
    return isValidFrameGraphBufferId(id) &&
           buffers.contains(id);
}

FrameGraphBufferId
FrameGraphResourceContainer::getBufferIdByName(
    std::string_view name) const {
    const auto found = name_to_id.find(std::string{name});
    return found != name_to_id.end()
               ? found->second
               : noFrameGraphBufferId();
}

const BufferWrapper &FrameGraphResourceContainer::buffer(std::string_view name) const {
    const auto id = getBufferIdByName(name);
    if (!isValidFrameGraphBufferId(id)) {
        throw std::runtime_error("Frame graph buffer not found: " + std::string{name});
    }
    return buffer(id);
}

const BufferWrapper &FrameGraphResourceContainer::buffer(
    FrameGraphBufferId id) const {
    return buffers.get(id).buffer;
}

vk::DeviceSize FrameGraphResourceContainer::bufferSize(std::string_view name) const {
    const auto id = getBufferIdByName(name);
    if (!isValidFrameGraphBufferId(id)) {
        throw std::runtime_error("Frame graph buffer not found: " + std::string{name});
    }
    return bufferSize(id);
}

vk::DeviceSize FrameGraphResourceContainer::bufferSize(
    FrameGraphBufferId id) const {
    return buffers.get(id).definition.size;
}

const FrameGraphBufferDefinition &
FrameGraphResourceContainer::definition(
    FrameGraphBufferId id) const {
    return buffers.get(id).definition;
}

vk::DescriptorBufferInfo FrameGraphResourceContainer::descriptorInfo(std::string_view name) const {
    const auto id = getBufferIdByName(name);
    if (!isValidFrameGraphBufferId(id)) {
        throw std::runtime_error(
            "Frame graph buffer not found: " + std::string{name});
    }
    return descriptorInfo(id);
}

vk::DescriptorBufferInfo FrameGraphResourceContainer::descriptorInfo(
    FrameGraphBufferId id) const {
    auto &record = buffers.get(id);
    return vk::DescriptorBufferInfo{record.buffer.buffer.get(), 0, record.definition.size};
}

bool FrameGraphResourceContainer::hasHostBufferSource(
    FrameGraphHostBufferSource source) const {
    return std::any_of(
        registration_order.begin(),
        registration_order.end(),
        [&](FrameGraphBufferId id) {
            if (!buffers.contains(id)) return false;
            const auto &candidate =
                buffers.get(id).definition.host_source;
            return candidate && *candidate == source;
        });
}

std::vector<
    FrameGraphResourceContainer::HostBufferTarget>
FrameGraphResourceContainer::hostBufferTargets(
    FrameGraphHostBufferSource source) const {
    std::vector<HostBufferTarget> result;
    for (const auto id : registration_order) {
        if (!buffers.contains(id)) continue;
        const auto &definition =
            buffers.get(id).definition;
        if (!definition.host_source ||
            *definition.host_source != source) {
            continue;
        }
        result.push_back(HostBufferTarget{
            .id = id,
            .name = definition.name,
            .size = definition.size,
            .source = source,
        });
    }
    return result;
}

void FrameGraphResourceContainer::writeHostBuffer(
    FrameGraphBufferId id,
    std::span<const std::byte> bytes,
    std::optional<HostBufferPopulation>
        population) {
    if (!buffers.contains(id)) {
        throw std::runtime_error(
            "Frame graph host buffer handle is unavailable");
    }
    auto &record = buffers.get(id);
    if (!record.definition.host_source) {
        throw std::runtime_error(
            "Frame graph buffer '" +
            record.definition.name +
            "' is not host sourced");
    }
    if (bytes.empty() ||
        bytes.size() > record.definition.size) {
        throw std::runtime_error(
            "Frame graph host write for '" +
            record.definition.name +
            "' requires 1.." +
            std::to_string(record.definition.size) +
            " bytes, received " +
            std::to_string(bytes.size()));
    }
    GET_MODULE(VulkanManageCore)
        .writeBuf(
            record.buffer, bytes.data(), 0,
            static_cast<vk::DeviceSize>(
                bytes.size()));
    if (population &&
        record.host_population != population) {
        const auto dropped =
            population->source_records -
            std::min(
                population->source_records,
                population->written_records);
        if (logger != nullptr && dropped != 0) {
            LOG_WARNING(
                logger,
                "Host buffer '{}' source '{}' truncated {} of {} records "
                "(written {})",
                record.definition.name,
                frameGraphHostBufferSourceName(
                    *record.definition.host_source),
                dropped,
                population->source_records,
                population->written_records);
        }
        record.host_population = *population;
    }
}

std::optional<
    FrameGraphResourceContainer::HostBufferPopulation>
FrameGraphResourceContainer::hostBufferPopulation(
    FrameGraphBufferId id) const {
    if (!buffers.contains(id)) {
        return std::nullopt;
    }
    return buffers.get(id).host_population;
}

FrameGraphResourceContainer::RegistrationCheckpoint
FrameGraphResourceContainer::checkpointRegistrations() const {
    return RegistrationCheckpoint{
        registration_order.size(), name_to_id};
}

void FrameGraphResourceContainer::rollbackRegistrations(
    RegistrationCheckpoint checkpoint) {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Frame graph buffer registration checkpoint is invalid");
    }
    while (registration_order.size() >
           checkpoint.registration_count) {
        (void)buffers.extract(
            registration_order.back(), false);
        registration_order.pop_back();
    }
    name_to_id = std::move(checkpoint.name_to_id);
}

std::vector<std::tuple<std::string, FrameGraphBufferId,
                       vk::DeviceSize>>
FrameGraphResourceContainer::registrationsSince(
    RegistrationCheckpoint checkpoint) const {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Frame graph buffer registration checkpoint is invalid");
    }
    std::vector<std::tuple<std::string, FrameGraphBufferId,
                           vk::DeviceSize>>
        result;
    result.reserve(registration_order.size() -
                   checkpoint.registration_count);
    for (std::size_t index = checkpoint.registration_count;
         index < registration_order.size(); ++index) {
        const auto id = registration_order[index];
        const auto &record = buffers.get(id);
        result.emplace_back(
            record.definition.name, id,
            record.definition.size);
    }
    return result;
}

void FrameGraphResourceContainer::hideRegistrationName(
    const std::string &name, FrameGraphBufferId expected) {
    const auto found = name_to_id.find(name);
    if (found != name_to_id.end() &&
        found->second == expected) {
        name_to_id.erase(found);
    }
}

void FrameGraphResourceContainer::retireRegistrations(
    const std::vector<FrameGraphBufferId> &ids) noexcept {
    for (const auto id : ids) {
        if (!buffers.contains(id)) continue;
        const auto name = buffers.get(id).definition.name;
        hideRegistrationName(name, id);
        auto retired = buffers.extract(id, false);
        std::erase(registration_order, id);
        if (retired) deferOrDestroy(std::move(*retired));
    }
}
} // namespace Pelican
