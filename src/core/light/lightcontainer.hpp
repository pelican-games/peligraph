#pragma once

#include "light.hpp"
#include "../container.hpp"
#include "../vkcore/buf.hpp"

#include <vector>
#include <nlohmann/json_fwd.hpp>
#include <vulkan/vulkan.hpp>
#include <unordered_map>

namespace Pelican
{
	DECLARE_MODULE(LightContainer)
	{
	public:
		LightContainer();
		~LightContainer();

		void Init();
		void Terminate();

		void Load(const nlohmann::json& json);
		void Update();
		void UpdateAnimation(float time);

		DirectionalLight* GetLight(const std::string& name);
		PointLight* GetPointLight(const std::string& name);
		SpotLight* GetSpotLight(const std::string& name);

		vk::DescriptorSetLayout GetDescriptorSetLayout() const { return m_DescriptorSetLayout.get(); }
		vk::DescriptorSet GetDescriptorSet() const { return m_DescriptorSet.get(); }

	private:
		std::vector<DirectionalLight> m_DirectionalLights;
		std::vector<DirectionalLight> m_OriginalDirectionalLights;
		std::unordered_map<std::string, uint32_t> m_LightNameMap;

		std::vector<PointLight> m_PointLights;
		std::vector<PointLight> m_OriginalPointLights;
		std::unordered_map<std::string, uint32_t> m_PointLightNameMap;

		std::vector<SpotLight> m_SpotLights;
		std::vector<SpotLight> m_OriginalSpotLights;
		std::unordered_map<std::string, uint32_t> m_SpotLightNameMap;

		BufferWrapper m_LightUBO;
		vk::UniqueDescriptorPool m_DescriptorPool;
		vk::UniqueDescriptorSetLayout m_DescriptorSetLayout;
		vk::UniqueDescriptorSet m_DescriptorSet;
	};
}
