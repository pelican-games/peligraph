#include "lightcontainer.hpp"
#include "../vkcore/core.hpp"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace Pelican
{
	namespace
	{
		glm::vec3 readVec3(const nlohmann::json& json, const std::string& field, const std::string& light_name)
		{
			if (!json.contains(field) || !json.at(field).is_array() || json.at(field).size() != 3)
			{
				const auto display_name = light_name.empty() ? std::string{"<unnamed>"} : light_name;
				throw std::runtime_error("Light '" + display_name + "' requires vec3 field: " + field);
			}

			const auto& value = json.at(field);
			return glm::vec3(
				value.at(0).get<float>(),
				value.at(1).get<float>(),
				value.at(2).get<float>()
			);
		}

		void registerLightName(std::unordered_map<std::string, uint32_t>& name_map, const std::string& name,
			uint32_t index, const std::string& light_type)
		{
			if (name.empty())
			{
				return;
			}
			if (name_map.find(name) != name_map.end())
			{
				throw std::runtime_error("Duplicate " + light_type + " light name: " + name);
			}
			name_map.emplace(name, index);
		}

		glm::vec3 safeLightDirection(const std::vector<DirectionalLight>& lights)
		{
			if (lights.empty() || glm::length(lights.front().direction) < 0.0001f)
			{
				return glm::normalize(glm::vec3{-0.5f, -1.0f, -0.5f});
			}
			return glm::normalize(lights.front().direction);
		}

		glm::mat4 vulkanOrtho(float left, float right, float bottom, float top, float z_near, float z_far)
		{
			glm::mat4 projection{1.0f};
			projection[0][0] = 2.0f / (right - left);
			projection[1][1] = 2.0f / (top - bottom);
			projection[2][2] = 1.0f / (z_near - z_far);
			projection[3][0] = -(right + left) / (right - left);
			projection[3][1] = -(top + bottom) / (top - bottom);
			projection[3][2] = z_near / (z_near - z_far);
			return projection;
		}
	}

	LightContainer::~LightContainer()
	{
	}

	glm::mat4 LightContainer::shadowViewProjection() const
	{
		const auto direction = safeLightDirection(m_DirectionalLights);
		const glm::vec3 center{0.0f, 0.0f, 0.0f};
		const glm::vec3 eye = center - direction * 10.0f;
		const glm::vec3 world_up =
			std::abs(glm::dot(direction, glm::vec3{0.0f, 1.0f, 0.0f})) > 0.95f
				? glm::vec3{0.0f, 0.0f, 1.0f}
				: glm::vec3{0.0f, 1.0f, 0.0f};
		const auto view = glm::lookAt(eye, center, world_up);
		const auto projection = vulkanOrtho(-6.0f, 6.0f, -6.0f, 6.0f, 0.1f, 30.0f);
		return projection * view;
	}

	LightContainer::LightContainer()
	{
		auto& vkcore = GET_MODULE(VulkanManageCore);

		// Create UBO
		m_LightUBO = vkcore.allocBuf(
			sizeof(LightUBO),
			vk::BufferUsageFlagBits::eUniformBuffer,
			vma::MemoryUsage::eAuto,
			vma::AllocationCreateFlagBits::eHostAccessSequentialWrite
		);
	}

	void LightContainer::load(const std::vector<LightLoadEntry>& lights)
	{
		m_DirectionalLights.clear();
		m_OriginalDirectionalLights.clear();
		m_LightNameMap.clear();
		m_PointLights.clear();
		m_OriginalPointLights.clear();
		m_PointLightNameMap.clear();
		m_SpotLights.clear();
		m_OriginalSpotLights.clear();
		m_SpotLightNameMap.clear();

		for (const auto& lightEntry : lights)
		{
			const auto& lightJson = lightEntry.component;
			const auto type = lightJson.value("type", "");
			if (type == "directional")
			{
				DirectionalLight light{};
				light.name = lightEntry.name;
				light.direction = readVec3(lightJson, "direction", light.name);
				light.intensity = lightJson.value("intensity", 1.0f);
				light.color = readVec3(lightJson, "color", light.name);

				registerLightName(m_LightNameMap, light.name, static_cast<uint32_t>(m_DirectionalLights.size()),
					"directional");
				m_DirectionalLights.push_back(light);
			}
			else if (type == "point")
			{
				PointLight light{};
				light.name = lightEntry.name;
				light.position = readVec3(lightJson, "position", light.name);
				light.intensity = lightJson.value("intensity", 1.0f);
				light.color = readVec3(lightJson, "color", light.name);

				registerLightName(m_PointLightNameMap, light.name, static_cast<uint32_t>(m_PointLights.size()),
					"point");
				m_PointLights.push_back(light);
			}
			else if (type == "spot")
			{
				SpotLight light{};
				light.name = lightEntry.name;
				light.position = readVec3(lightJson, "position", light.name);
				light.direction = readVec3(lightJson, "direction", light.name);
				light.intensity = lightJson.value("intensity", 1.0f);
				light.innerConeAngle = lightJson.value("innerConeAngle", 12.5f);
				light.outerConeAngle = lightJson.value("outerConeAngle", 17.5f);
				light.color = readVec3(lightJson, "color", light.name);

				registerLightName(m_SpotLightNameMap, light.name, static_cast<uint32_t>(m_SpotLights.size()),
					"spot");
				m_SpotLights.push_back(light);
			}
			else
			{
				throw std::runtime_error("Unknown light type: " + type);
			}
		}

		m_OriginalDirectionalLights = m_DirectionalLights;
		m_OriginalPointLights = m_PointLights;
		m_OriginalSpotLights = m_SpotLights;
	}

	DirectionalLight* LightContainer::getLight(const std::string& name)
		    {
		        auto it = m_LightNameMap.find(name);
		        if (it != m_LightNameMap.end())
		        {
		            return &m_DirectionalLights[it->second];
		        }
		        return nullptr;
		    }
		
	PointLight* LightContainer::getPointLight(const std::string& name)
		    	{
		    		auto it = m_PointLightNameMap.find(name);
		    		if (it != m_PointLightNameMap.end())
		    		{
		    			return &m_PointLights[it->second];
		    		}
		    		return nullptr;
		    	}
		
	SpotLight* LightContainer::getSpotLight(const std::string& name)
		    	{
		    		auto it = m_SpotLightNameMap.find(name);
		    		if (it != m_SpotLightNameMap.end())
		    		{
		    			return &m_SpotLights[it->second];
		    		}
		    		return nullptr;
		    	}   
		
	void LightContainer::updateAnimation(float time)
		    	{
		    		// Rotate the key light
		if (DirectionalLight* keyLight = getLight("KeyLight"))
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
		if (DirectionalLight* fillLight = getLight("FillLight"))
		    		{
		    			auto it = m_LightNameMap.find("FillLight");
		    			if (it != m_LightNameMap.end())
		    			{
		    				const auto& originalLight = m_OriginalDirectionalLights[it->second];
		    				fillLight->intensity = originalLight.intensity * (0.5f + 0.5f * sinf(time * 5.0f));
		    			}
		    		}
		
		    		// Orbit a point light
		if (PointLight* pointLight = getPointLight("PointLight1"))
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
		if (SpotLight* spotLight = getSpotLight("SpotLight1"))
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
		
	void LightContainer::update()
		    	{
		    		LightUBO ubo{};
                ubo.directionalLightCount =
                    std::min<uint32_t>(static_cast<uint32_t>(m_DirectionalLights.size()), MAX_DIRECTIONAL_LIGHTS);
                for (size_t i = 0; i < ubo.directionalLightCount; ++i)
		    		{
		    			ubo.directionalLights[i].direction = m_DirectionalLights[i].direction;
		    			ubo.directionalLights[i].intensity = m_DirectionalLights[i].intensity;
		    			ubo.directionalLights[i].color = m_DirectionalLights[i].color;
		    		}
                ubo.pointLightCount =
                    std::min<uint32_t>(static_cast<uint32_t>(m_PointLights.size()), MAX_POINT_LIGHTS);
                for (size_t i = 0; i < ubo.pointLightCount; ++i)
		    		{
		    			ubo.pointLights[i].position = m_PointLights[i].position;
		    			ubo.pointLights[i].intensity = m_PointLights[i].intensity;
		    			ubo.pointLights[i].color = m_PointLights[i].color;
		    		}
                ubo.spotLightCount =
                    std::min<uint32_t>(static_cast<uint32_t>(m_SpotLights.size()), MAX_SPOT_LIGHTS);
                for (size_t i = 0; i < ubo.spotLightCount; ++i)
		    		{
		    			ubo.spotLights[i].position = m_SpotLights[i].position;
		    			ubo.spotLights[i].direction = m_SpotLights[i].direction;
		    			ubo.spotLights[i].intensity = m_SpotLights[i].intensity;
		    			ubo.spotLights[i].color = m_SpotLights[i].color;
		    			ubo.spotLights[i].innerConeAngle = cos(glm::radians(m_SpotLights[i].innerConeAngle));
		    			ubo.spotLights[i].outerConeAngle = cos(glm::radians(m_SpotLights[i].outerConeAngle));
		    		}
				ubo.shadowViewProjection = shadowViewProjection();
		
		    		GET_MODULE(VulkanManageCore).writeBuf(m_LightUBO, &ubo, 0, sizeof(ubo));
		    	}
		    }
