#pragma once

#include "framegraphbufferdefinition.hpp"
#include "renderingpass.hpp"
#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include "../vkcore/buf.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(FrameGraphResourceContainer) {
  public:
    struct HostBufferPopulation {
        std::uint64_t source_records = 0;
        std::uint64_t written_records = 0;

        bool operator==(
            const HostBufferPopulation &) const = default;
    };

  private:
    struct BufferRecord {
        FrameGraphBufferDefinition definition;
        BufferWrapper buffer;
        std::vector<float> runtime_values;
        bool runtime_values_dirty = false;
        std::optional<HostBufferPopulation>
            host_population;
    };

    ResourceContainer<FrameGraphBufferId, BufferRecord> buffers;
    std::unordered_map<std::string, FrameGraphBufferId> name_to_id;
    std::vector<FrameGraphBufferId> registration_order;

  public:
    struct HostBufferTarget {
        FrameGraphBufferId id = noFrameGraphBufferId();
        std::string name;
        vk::DeviceSize size = 0;
        FrameGraphHostBufferSource source =
            FrameGraphHostBufferSource::scene_lights_v2;
    };

    struct RegistrationCheckpoint {
        std::size_t registration_count = 0;
        std::unordered_map<std::string, FrameGraphBufferId>
            name_to_id;
    };

    FrameGraphResourceContainer();
    ~FrameGraphResourceContainer();

    void registerBuffers(const std::vector<FrameGraphBufferDefinition> &definitions);
    bool hasBuffer(std::string_view name) const;
    // Owner-thread staging; upload at the renderer's existing host-write phase.
    bool stageRuntimeValues(std::string_view name, std::span<const float> values);
    bool stageRuntimeValues(FrameGraphBufferId id, std::span<const float> values);
    void uploadRuntimeValues();
    // Copy persistent contents into private replacement buffers. The caller
    // supplies unchanged declarations and retains both renderer generations.
    void copyBufferState(const std::vector<std::pair<FrameGraphBufferId, FrameGraphBufferId>> &copies);
    bool hasBuffer(FrameGraphBufferId id) const;
    FrameGraphBufferId getBufferIdByName(std::string_view name) const;
    const BufferWrapper &buffer(std::string_view name) const;
    const BufferWrapper &buffer(FrameGraphBufferId id) const;
    vk::DeviceSize bufferSize(std::string_view name) const;
    vk::DeviceSize bufferSize(FrameGraphBufferId id) const;
    const FrameGraphBufferDefinition &definition(
        FrameGraphBufferId id) const;
    vk::DescriptorBufferInfo descriptorInfo(std::string_view name) const;
    vk::DescriptorBufferInfo descriptorInfo(FrameGraphBufferId id) const;
    bool hasHostBufferSource(
        FrameGraphHostBufferSource source) const;
    std::vector<HostBufferTarget> hostBufferTargets(
        FrameGraphHostBufferSource source) const;
    void writeHostBuffer(
        FrameGraphBufferId id,
        std::span<const std::byte> bytes,
        std::optional<HostBufferPopulation>
            population = std::nullopt);
    std::optional<HostBufferPopulation>
    hostBufferPopulation(
        FrameGraphBufferId id) const;

    RegistrationCheckpoint checkpointRegistrations() const;
    void rollbackRegistrations(RegistrationCheckpoint checkpoint);
    std::vector<std::tuple<std::string, FrameGraphBufferId,
                           vk::DeviceSize>>
    registrationsSince(RegistrationCheckpoint checkpoint) const;
    std::size_t registrationCount() const noexcept {
        return registration_order.size();
    }
    std::unordered_map<std::string, FrameGraphBufferId>
    currentNameBindings() const {
        return name_to_id;
    }
    void hideRegistrationName(const std::string &name,
                              FrameGraphBufferId expected);
    void retireRegistrations(
        const std::vector<FrameGraphBufferId> &ids) noexcept;
};

} // namespace Pelican
