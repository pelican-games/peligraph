#include "lightcontainer.hpp"
#include "../log.hpp"
#include "../userpublic/color.hpp"
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

		glm::vec3 readSrgbColor(const nlohmann::json& json, const std::string& light_name)
		{
			const auto encoded = readVec3(json, "color", light_name);
			const auto linear = Pelican::srgb(encoded.r, encoded.g, encoded.b, 1.0f);
			return glm::vec3{linear};
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

		std::string capWarning(const std::string& type, const std::string& name, size_t ordinal, size_t cap)
		{
			const auto display_name = name.empty() ? std::string{"<unnamed>"} : name;
			return "Light cap exceeded: " + type + " light #" + std::to_string(ordinal) + " '" +
				display_name + "' will not be rendered (cap " + std::to_string(cap) + ")";
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

	std::vector<std::string> collectLightCapWarnings(const std::vector<LightLoadEntry>& lights)
	{
		std::vector<std::string> warnings;
		size_t directional_count = 0;
		size_t point_count = 0;
		size_t spot_count = 0;
		for (const auto& entry : lights)
		{
			const auto type = entry.component.value("type", "");
			if (type == "directional")
			{
				if (++directional_count > MAX_DIRECTIONAL_LIGHTS)
				{
					warnings.push_back(capWarning(type, entry.name, directional_count, MAX_DIRECTIONAL_LIGHTS));
				}
			}
			else if (type == "point")
			{
				if (++point_count > MAX_POINT_LIGHTS)
				{
					warnings.push_back(capWarning(type, entry.name, point_count, MAX_POINT_LIGHTS));
				}
			}
			else if (type == "spot")
			{
				if (++spot_count > MAX_SPOT_LIGHTS)
				{
					warnings.push_back(capWarning(type, entry.name, spot_count, MAX_SPOT_LIGHTS));
				}
			}
		}
		return warnings;
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
		m_LightNameMap.clear();
		m_PointLights.clear();
		m_PointLightNameMap.clear();
		m_SpotLights.clear();
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
				light.color = readSrgbColor(lightJson, light.name);

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
				light.color = readSrgbColor(lightJson, light.name);

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
				light.color = readSrgbColor(lightJson, light.name);

				registerLightName(m_SpotLightNameMap, light.name, static_cast<uint32_t>(m_SpotLights.size()),
					"spot");
				m_SpotLights.push_back(light);
			}
			else
			{
				throw std::runtime_error("Unknown light type: " + type);
			}
		}

		if (logger != nullptr)
		{
			for (const auto& warning : collectLightCapWarnings(lights))
			{
				LOG_WARNING(logger, "{}", warning);
			}
		}
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
		
	bool LightContainer::setDirectionalLightDirection(const std::string& name, glm::vec3 direction)
	{
		if (auto* light = getLight(name))
		{
			light->direction = direction;
			return true;
		}
		return false;
	}

	bool LightContainer::setDirectionalLightIntensity(const std::string& name, float intensity)
	{
		if (auto* light = getLight(name))
		{
			light->intensity = intensity;
			return true;
		}
		return false;
	}

	bool LightContainer::setPointLightPosition(const std::string& name, glm::vec3 position)
	{
		if (auto* light = getPointLight(name))
		{
			light->position = position;
			return true;
		}
		return false;
	}

	bool LightContainer::setSpotLightDirection(const std::string& name, glm::vec3 direction)
	{
		if (auto* light = getSpotLight(name))
		{
			light->direction = direction;
			return true;
		}
		return false;
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
