#pragma once

#include "directionalshadowdata.hpp"
#include "light.hpp"
#include "lightinventory.hpp"
#include "../container.hpp"
#include "../loader/componentcodec.hpp"
#include "../vkcore/buf.hpp"

#include <string>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <unordered_map>

namespace Pelican
{
	struct LightLoadEntry
	{
		std::string name;
		LightCodecData component;
	};

	std::vector<std::string> collectLightCapWarnings(const std::vector<LightLoadEntry>& lights);

	DECLARE_MODULE(LightContainer)
	{
	public:
		struct PreparedLoad
		{
			std::vector<DirectionalLight> directional_lights;
			std::unordered_map<std::string, uint32_t> directional_names;
			std::vector<PointLight> point_lights;
			std::unordered_map<std::string, uint32_t> point_names;
			std::vector<SpotLight> spot_lights;
			std::unordered_map<std::string, uint32_t> spot_names;
			std::vector<std::string> warnings;
		};

		LightContainer();
		~LightContainer();

		static PreparedLoad prepareLoad(const std::vector<LightLoadEntry>& lights);
		PreparedLoad snapshotPrepared() const;
		void publishPrepared(PreparedLoad&& prepared) noexcept;
		void load(const std::vector<LightLoadEntry>& lights);
		void update();
		void update(
			const SkyAmbientLighting&
				sky_ambient);
		void update(const glm::mat4& shadow_view_projection);
		void update(
			std::span<const glm::mat4>
				shadow_view_projections,
			std::span<const float>
				cascade_far_distances);
		void update(
			std::span<const glm::mat4>
				shadow_view_projections,
			std::span<const float>
				cascade_far_distances,
			const SkyAmbientLighting&
				sky_ambient);
		void updateDirectionalShadows(
			std::span<const glm::mat4>
				shadow_view_projections,
			std::span<const float>
				cascade_far_distances,
			std::uint32_t cascade_count,
			std::uint32_t shadow_light_count,
			const SkyAmbientLighting&
				sky_ambient);

		bool setDirectionalLightDirection(const std::string& name, glm::vec3 direction);
		bool setDirectionalLightIntensity(const std::string& name, float intensity);
		bool setPointLightPosition(const std::string& name, glm::vec3 position);
		bool setSpotLightDirection(const std::string& name, glm::vec3 direction);

		DirectionalShadowView directionalShadowView() const;
		DirectionalShadowView directionalShadowView(
			std::uint32_t directional_light_index) const;
		glm::vec3 directionalShadowDirection() const;
		glm::vec3 directionalShadowDirection(
			std::uint32_t directional_light_index) const;
		glm::mat4 shadowViewProjection() const;
		std::size_t directionalLightCount() const noexcept {
			return m_DirectionalLights.size();
		}
		const BufferWrapper& lightBuffer() const { return m_LightUBO; }
		const BufferWrapper& directionalShadowBuffer() const {
			return m_DirectionalShadowBuffer;
		}
		std::size_t directionalShadowDataElementCount() const noexcept {
			return m_DirectionalShadowDataElementCount;
		}
		PackedLightInventoryV2 lightInventoryV2(
			vk::DeviceSize byte_capacity) const;

	private:
		DirectionalLight* getLight(const std::string& name);
		PointLight* getPointLight(const std::string& name);
		SpotLight* getSpotLight(const std::string& name);
		void updateImpl(
			std::span<const glm::mat4>
				shadow_view_projections,
			std::span<const float>
				cascade_far_distances,
			std::uint32_t cascade_count,
			std::uint32_t shadow_light_count,
			const SkyAmbientLighting&
				sky_ambient);
		void ensureDirectionalShadowBufferCapacity(
			vk::DeviceSize required_bytes);

		std::vector<DirectionalLight> m_DirectionalLights;
		std::unordered_map<std::string, uint32_t> m_LightNameMap;

		std::vector<PointLight> m_PointLights;
		std::unordered_map<std::string, uint32_t> m_PointLightNameMap;

		std::vector<SpotLight> m_SpotLights;
		std::unordered_map<std::string, uint32_t> m_SpotLightNameMap;

		BufferWrapper m_LightUBO;
		BufferWrapper m_DirectionalShadowBuffer;
		vk::DeviceSize m_DirectionalShadowBufferCapacity = 0;
		std::size_t m_DirectionalShadowDataElementCount = 0;
	};
}
