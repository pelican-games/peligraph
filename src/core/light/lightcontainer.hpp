#pragma once

#include "light.hpp"
#include "lightinventory.hpp"
#include "../container.hpp"
#include "../vkcore/buf.hpp"

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <vulkan/vulkan.hpp>
#include <unordered_map>

namespace Pelican
{
	struct LightLoadEntry
	{
		std::string name;
		nlohmann::json component;
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
		void update(const glm::mat4& shadow_view_projection);

		bool setDirectionalLightDirection(const std::string& name, glm::vec3 direction);
		bool setDirectionalLightIntensity(const std::string& name, float intensity);
		bool setPointLightPosition(const std::string& name, glm::vec3 position);
		bool setSpotLightDirection(const std::string& name, glm::vec3 direction);

		DirectionalShadowView directionalShadowView() const;
		glm::mat4 shadowViewProjection() const;
		const BufferWrapper& lightBuffer() const { return m_LightUBO; }
		PackedLightInventoryV2 lightInventoryV2(
			vk::DeviceSize byte_capacity) const;

	private:
		DirectionalLight* getLight(const std::string& name);
		PointLight* getPointLight(const std::string& name);
		SpotLight* getSpotLight(const std::string& name);

		std::vector<DirectionalLight> m_DirectionalLights;
		std::unordered_map<std::string, uint32_t> m_LightNameMap;

		std::vector<PointLight> m_PointLights;
		std::unordered_map<std::string, uint32_t> m_PointLightNameMap;

		std::vector<SpotLight> m_SpotLights;
		std::unordered_map<std::string, uint32_t> m_SpotLightNameMap;

		BufferWrapper m_LightUBO;
	};
}
