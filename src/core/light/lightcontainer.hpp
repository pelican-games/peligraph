#pragma once

#include "light.hpp"
#include "../container.hpp"
#include "../vkcore/buf.hpp"

#include <string>
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

		void load(const nlohmann::json& json);
		void update();
		void updateAnimation(float time);

		void bindResource(vk::CommandBuffer cmd_buf, vk::PipelineLayout pipeline_layout, uint32_t set_number) const;

		vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_DescriptorSetLayout.get(); }

	private:
		DirectionalLight* getLight(const std::string& name);
		PointLight* getPointLight(const std::string& name);
		SpotLight* getSpotLight(const std::string& name);

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
