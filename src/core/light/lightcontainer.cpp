#include "lightcontainer.hpp"
#include "../vkcore/core.hpp"
#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Pelican
{
	namespace
	{
		static vk::UniqueDescriptorPool createDescriptorPool(vk::Device device) {
			vk::DescriptorPoolSize pool_size[1];
			pool_size[0].type = vk::DescriptorType::eUniformBuffer;
			pool_size[0].descriptorCount = 1;

			vk::DescriptorPoolCreateInfo create_info;
			create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
			create_info.maxSets = 1;
			create_info.setPoolSizes(pool_size);
			return device.createDescriptorPoolUnique(create_info);
		}
	}

	LightContainer::LightContainer()
	{
		Init();
	}

	LightContainer::~LightContainer()
	{
		Terminate();
	}

	void LightContainer::Init()
	{
		auto& vkcore = GET_MODULE(VulkanManageCore);
		auto device = vkcore.getDevice();

		// Create UBO
		m_LightUBO = vkcore.allocBuf(
			sizeof(LightUBO),
			vk::BufferUsageFlagBits::eUniformBuffer,
			vma::MemoryUsage::eAuto,
			vma::AllocationCreateFlagBits::eHostAccessSequentialWrite
		);

		// Create Descriptor Set Layout
		vk::DescriptorSetLayoutBinding uboLayoutBinding{};
		uboLayoutBinding.binding = 0;
		uboLayoutBinding.descriptorType = vk::DescriptorType::eUniformBuffer;
		uboLayoutBinding.descriptorCount = 1;
		uboLayoutBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;

		vk::DescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.setBindings(uboLayoutBinding);

		m_DescriptorSetLayout = device.createDescriptorSetLayoutUnique(layoutInfo);

		// Create Descriptor Set
		m_DescriptorPool = createDescriptorPool(device);
		vk::DescriptorSetAllocateInfo allocInfo{};
		allocInfo.descriptorPool = m_DescriptorPool.get();
		allocInfo.setSetLayouts(m_DescriptorSetLayout.get());

		m_DescriptorSet = std::move(device.allocateDescriptorSetsUnique(allocInfo)[0]);

		// Update Descriptor Set
		vk::DescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = m_LightUBO.buffer.get();
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(LightUBO);

		vk::WriteDescriptorSet descriptorWrite{};
		descriptorWrite.dstSet = m_DescriptorSet.get();
		descriptorWrite.dstBinding = 0;
		descriptorWrite.dstArrayElement = 0;
		descriptorWrite.descriptorType = vk::DescriptorType::eUniformBuffer;
		descriptorWrite.setBufferInfo(bufferInfo);

		device.updateDescriptorSets(descriptorWrite, nullptr);
	}

	void LightContainer::Terminate()
	{
		// vk::Unique handles will automatically clean up resources.
	}

	void LightContainer::Load(const nlohmann::json& json)
	{
		if (json.find("lights") == json.end())
		{
			return;
		}

		const auto& lightsJson = json["lights"];
		for (const auto& lightJson : lightsJson)
		{
			const auto type = lightJson.value("type", "");
			if (type == "directional")
			{
				DirectionalLight light{};
				light.name = lightJson.value("name", "");
				light.direction = glm::vec3(
					lightJson["direction"][0].get<float>(),
					lightJson["direction"][1].get<float>(),
					lightJson["direction"][2].get<float>()
				);
								light.intensity = lightJson.value("intensity", 1.0f);
								light.color = glm::vec3(
									lightJson["color"][0].get<float>(),
									lightJson["color"][1].get<float>(),
									lightJson["color"][2].get<float>()
								);
				
								if (!light.name.empty())
								{
									m_LightNameMap[light.name] = static_cast<uint32_t>(m_DirectionalLights.size());
								}
								m_DirectionalLights.push_back(light);
							}
							else if (type == "point")
							{
								PointLight light{};
								light.name = lightJson.value("name", "");
								light.position = glm::vec3(
									lightJson["position"][0].get<float>(),
									lightJson["position"][1].get<float>(),
									lightJson["position"][2].get<float>()
								);
								light.intensity = lightJson.value("intensity", 1.0f);
								light.color = glm::vec3(
									lightJson["color"][0].get<float>(),
									lightJson["color"][1].get<float>(),
									lightJson["color"][2].get<float>()
								);
				
												if (!light.name.empty())
												{
													m_PointLightNameMap[light.name] = static_cast<uint32_t>(m_PointLights.size());
												}
												m_PointLights.push_back(light);
											}
											else if (type == "spot")
											{
												SpotLight light{};
												light.name = lightJson.value("name", "");
												light.position = glm::vec3(
													lightJson["position"][0].get<float>(),
													lightJson["position"][1].get<float>(),
													lightJson["position"][2].get<float>()
												);
												light.direction = glm::vec3(
													lightJson["direction"][0].get<float>(),
													lightJson["direction"][1].get<float>(),
													lightJson["direction"][2].get<float>()
												);
												light.intensity = lightJson.value("intensity", 1.0f);
												light.innerConeAngle = lightJson.value("innerConeAngle", 12.5f);
												light.outerConeAngle = lightJson.value("outerConeAngle", 17.5f);
												light.color = glm::vec3(
													lightJson["color"][0].get<float>(),
													lightJson["color"][1].get<float>(),
													lightJson["color"][2].get<float>()
												);
								
												if (!light.name.empty())
												{
													m_SpotLightNameMap[light.name] = static_cast<uint32_t>(m_SpotLights.size());
												}
												m_SpotLights.push_back(light);
											}
										}
										m_OriginalDirectionalLights = m_DirectionalLights;
										m_OriginalPointLights = m_PointLights;
										m_OriginalSpotLights = m_SpotLights;
									}		    DirectionalLight* LightContainer::GetLight(const std::string& name)
		    {
		        auto it = m_LightNameMap.find(name);
		        if (it != m_LightNameMap.end())
		        {
		            return &m_DirectionalLights[it->second];
		        }
		        return nullptr;
		    }
		
		    	PointLight* LightContainer::GetPointLight(const std::string& name)
		    	{
		    		auto it = m_PointLightNameMap.find(name);
		    		if (it != m_PointLightNameMap.end())
		    		{
		    			return &m_PointLights[it->second];
		    		}
		    		return nullptr;
		    	}
		
		    	SpotLight* LightContainer::GetSpotLight(const std::string& name)
		    	{
		    		auto it = m_SpotLightNameMap.find(name);
		    		if (it != m_SpotLightNameMap.end())
		    		{
		    			return &m_SpotLights[it->second];
		    		}
		    		return nullptr;
		    	}   
		
		    	void LightContainer::UpdateAnimation(float time)
		    	{
		    		// Rotate the key light
		    		if (DirectionalLight* keyLight = GetLight("KeyLight"))
		    		{
		    			auto it = m_LightNameMap.find("KeyLight");
		    			if (it != m_LightNameMap.end())
		    			{
		    				const auto& originalLight = m_OriginalDirectionalLights[it->second];
		    				glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.0f, 1.0f, 0.0f));
		    				keyLight->direction = glm::vec3(rotation * glm::vec4(originalLight.direction, 0.0f));
		    			}
		    		}
		
		    		// Make the fill light blink
		    		if (DirectionalLight* fillLight = GetLight("FillLight"))
		    		{
		    			auto it = m_LightNameMap.find("FillLight");
		    			if (it != m_LightNameMap.end())
		    			{
		    				const auto& originalLight = m_OriginalDirectionalLights[it->second];
		    				fillLight->intensity = originalLight.intensity * (0.5f + 0.5f * sinf(time * 5.0f));
		    			}
		    		}
		
		    		// Orbit a point light
		    		if (PointLight* pointLight = GetPointLight("PointLight1"))
		    		{
		    			auto it = m_PointLightNameMap.find("PointLight1");
		    			if (it != m_PointLightNameMap.end())
		    			{
		    				const auto& originalLight = m_OriginalPointLights[it->second];
		    				pointLight->position.x = originalLight.position.x + cos(time) * 2.0f;
		    				pointLight->position.z = originalLight.position.z + sin(time) * 2.0f;
		    			}
		    		}
		    		// Swing a spotlight
		    		if (SpotLight* spotLight = GetSpotLight("SpotLight1"))
		    		{
		    			auto it = m_SpotLightNameMap.find("SpotLight1");
		    			if (it != m_SpotLightNameMap.end())
		    			{
		    				const auto& originalLight = m_OriginalSpotLights[it->second];
		    				glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), sinf(time * 0.5f) * 0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
		    				spotLight->direction = glm::vec3(rotation * glm::vec4(originalLight.direction, 0.0f));
		    			}
		    		}
		    	}
		
		    	void LightContainer::Update()
		    	{
		    		LightUBO ubo{};
		    		ubo.directionalLightCount = static_cast<uint32_t>(m_DirectionalLights.size());
		    		for (size_t i = 0; i < m_DirectionalLights.size() && i < MAX_DIRECTIONAL_LIGHTS; ++i)
		    		{
		    			ubo.directionalLights[i].direction = m_DirectionalLights[i].direction;
		    			ubo.directionalLights[i].intensity = m_DirectionalLights[i].intensity;
		    			ubo.directionalLights[i].color = m_DirectionalLights[i].color;
		    		}
		    		ubo.pointLightCount = static_cast<uint32_t>(m_PointLights.size());
		    		for (size_t i = 0; i < m_PointLights.size() && i < MAX_POINT_LIGHTS; ++i)
		    		{
		    			ubo.pointLights[i].position = m_PointLights[i].position;
		    			ubo.pointLights[i].intensity = m_PointLights[i].intensity;
		    			ubo.pointLights[i].color = m_PointLights[i].color;
		    		}
		    		ubo.spotLightCount = static_cast<uint32_t>(m_SpotLights.size());
		    		for (size_t i = 0; i < m_SpotLights.size() && i < MAX_SPOT_LIGHTS; ++i)
		    		{
		    			ubo.spotLights[i].position = m_SpotLights[i].position;
		    			ubo.spotLights[i].direction = m_SpotLights[i].direction;
		    			ubo.spotLights[i].intensity = m_SpotLights[i].intensity;
		    			ubo.spotLights[i].color = m_SpotLights[i].color;
		    			ubo.spotLights[i].innerConeAngle = cos(glm::radians(m_SpotLights[i].innerConeAngle));
		    			ubo.spotLights[i].outerConeAngle = cos(glm::radians(m_SpotLights[i].outerConeAngle));
		    		}
		
		    		GET_MODULE(VulkanManageCore).writeBuf(m_LightUBO, &ubo, 0, sizeof(ubo));
		    	}
		    }
