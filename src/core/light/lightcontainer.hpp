#pragma once

#include "light.hpp"
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
		LightContainer();
		~LightContainer();

		void load(const std::vector<LightLoadEntry>& lights);
		void update();

		bool setDirectionalLightDirection(const std::string& name, glm::vec3 direction);
		bool setDirectionalLightIntensity(const std::string& name, float intensity);
		bool setPointLightPosition(const std::string& name, glm::vec3 position);
		bool setSpotLightDirection(const std::string& name, glm::vec3 direction);

		glm::mat4 shadowViewProjection() const;
		const BufferWrapper& lightBuffer() const { return m_LightUBO; }

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
