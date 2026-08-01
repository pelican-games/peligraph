#include "lightcontainer.hpp"
#include "../log.hpp"
#include "../userpublic/color.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
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

		glm::vec3 fallbackLightDirection()
		{
			return glm::normalize(
				glm::vec3{-0.5f, -1.0f, -0.5f});
		}

		glm::vec3 safeLightDirection(
			const std::vector<DirectionalLight>& lights,
			std::size_t index)
		{
			if (index >= lights.size())
			{
				throw std::out_of_range(
					"directional shadow light index is out of range");
			}
			if (glm::length(lights[index].direction) < 0.0001f)
			{
				return fallbackLightDirection();
			}
			return glm::normalize(lights[index].direction);
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

	DirectionalShadowView LightContainer::directionalShadowView() const
	{
		const auto direction = directionalShadowDirection();
		const glm::vec3 center{0.0f, 0.0f, 0.0f};
		const glm::vec3 eye = center - direction * 10.0f;
		const glm::vec3 world_up =
			std::abs(glm::dot(direction, glm::vec3{0.0f, 1.0f, 0.0f})) > 0.95f
				? glm::vec3{0.0f, 0.0f, 1.0f}
				: glm::vec3{0.0f, 1.0f, 0.0f};
		const auto view = glm::lookAt(eye, center, world_up);
		const auto projection = vulkanOrtho(-6.0f, 6.0f, -6.0f, 6.0f, 0.1f, 30.0f);
		return DirectionalShadowView{
			.view = view,
			.projection = projection,
			.camera_position = eye,
		};
	}

	DirectionalShadowView LightContainer::directionalShadowView(
		std::uint32_t directional_light_index) const
	{
		const auto direction =
			directionalShadowDirection(
				directional_light_index);
		const glm::vec3 center{0.0f, 0.0f, 0.0f};
		const glm::vec3 eye = center - direction * 10.0f;
		const glm::vec3 world_up =
			std::abs(glm::dot(direction, glm::vec3{0.0f, 1.0f, 0.0f})) > 0.95f
				? glm::vec3{0.0f, 0.0f, 1.0f}
				: glm::vec3{0.0f, 1.0f, 0.0f};
		const auto view = glm::lookAt(eye, center, world_up);
		const auto projection = vulkanOrtho(-6.0f, 6.0f, -6.0f, 6.0f, 0.1f, 30.0f);
		return DirectionalShadowView{
			.view = view,
			.projection = projection,
			.camera_position = eye,
		};
	}

	glm::vec3 LightContainer::directionalShadowDirection() const
	{
		return m_DirectionalLights.empty()
			? fallbackLightDirection()
			: safeLightDirection(
				m_DirectionalLights, 0);
	}

	glm::vec3 LightContainer::directionalShadowDirection(
		std::uint32_t directional_light_index) const
	{
		return safeLightDirection(
			m_DirectionalLights,
			directional_light_index);
	}

	glm::mat4 LightContainer::shadowViewProjection() const
	{
		const auto shadow_view = directionalShadowView();
		return shadow_view.projection * shadow_view.view;
	}

	PackedLightInventoryV2
	LightContainer::lightInventoryV2(
		vk::DeviceSize byte_capacity) const
	{
		return packLightInventoryV2(
			m_DirectionalLights,
			m_PointLights,
			m_SpotLights,
			byte_capacity);
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
		const auto empty_shadow_data =
			packDirectionalShadowDataV1(
				std::span<const std::uint32_t>{},
				1,
				std::span<const glm::mat4>{});
		m_DirectionalShadowBufferCapacity =
			empty_shadow_data.elements.size() *
			sizeof(glm::uvec4);
		m_DirectionalShadowBuffer = vkcore.allocBuf(
			m_DirectionalShadowBufferCapacity,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vma::MemoryUsage::eAuto,
			vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
		m_DirectionalShadowDataElementCount =
			empty_shadow_data.elements.size();
		vkcore.writeBuf(
			m_DirectionalShadowBuffer,
			empty_shadow_data.elements.data(), 0,
			m_DirectionalShadowBufferCapacity);
	}

	LightContainer::PreparedLoad LightContainer::prepareLoad(const std::vector<LightLoadEntry>& lights)
	{
		PreparedLoad prepared;

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

				registerLightName(prepared.directional_names, light.name, static_cast<uint32_t>(prepared.directional_lights.size()),
					"directional");
				prepared.directional_lights.push_back(light);
			}
			else if (type == "point")
			{
				PointLight light{};
				light.name = lightEntry.name;
				light.position = readVec3(lightJson, "position", light.name);
				light.intensity = lightJson.value("intensity", 1.0f);
				light.color = readSrgbColor(lightJson, light.name);

				registerLightName(prepared.point_names, light.name, static_cast<uint32_t>(prepared.point_lights.size()),
					"point");
				prepared.point_lights.push_back(light);
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

				registerLightName(prepared.spot_names, light.name, static_cast<uint32_t>(prepared.spot_lights.size()),
					"spot");
				prepared.spot_lights.push_back(light);
			}
			else
			{
				throw std::runtime_error("Unknown light type: " + type);
			}
		}

		prepared.warnings = collectLightCapWarnings(lights);
		return prepared;
	}

	LightContainer::PreparedLoad LightContainer::snapshotPrepared() const
	{
		return PreparedLoad{
			.directional_lights = m_DirectionalLights,
			.directional_names = m_LightNameMap,
			.point_lights = m_PointLights,
			.point_names = m_PointLightNameMap,
			.spot_lights = m_SpotLights,
			.spot_names = m_SpotLightNameMap,
		};
	}

	void LightContainer::publishPrepared(PreparedLoad&& prepared) noexcept
	{
		m_DirectionalLights.swap(prepared.directional_lights);
		m_LightNameMap.swap(prepared.directional_names);
		m_PointLights.swap(prepared.point_lights);
		m_PointLightNameMap.swap(prepared.point_names);
		m_SpotLights.swap(prepared.spot_lights);
		m_SpotLightNameMap.swap(prepared.spot_names);
	}

	void LightContainer::load(const std::vector<LightLoadEntry>& lights)
	{
		auto prepared = prepareLoad(lights);
		if (logger != nullptr)
		{
			for (const auto& warning : prepared.warnings)
			{
				LOG_WARNING(logger, "{}", warning);
			}
		}
		publishPrepared(std::move(prepared));
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
		update(SkyAmbientLighting{});
	}

	void LightContainer::update(
		const SkyAmbientLighting&
			sky_ambient)
	{
		const std::array projections{
			shadowViewProjection()};
		updateImpl(
			projections, {}, 1, 0,
			sky_ambient);
	}

	void LightContainer::update(
		const glm::mat4& shadow_view_projection)
	{
		const std::array projections{
			shadow_view_projection};
		updateImpl(
			projections, {}, 1,
			m_DirectionalLights.empty() ? 0u : 1u,
			SkyAmbientLighting{});
	}

	void LightContainer::update(
		std::span<const glm::mat4>
			shadow_view_projections,
		std::span<const float>
			cascade_far_distances)
	{
		updateImpl(
			shadow_view_projections,
			cascade_far_distances,
			static_cast<std::uint32_t>(
				shadow_view_projections.size()),
			m_DirectionalLights.empty() ? 0u : 1u,
			SkyAmbientLighting{});
	}

	void LightContainer::update(
		std::span<const glm::mat4>
			shadow_view_projections,
		std::span<const float>
			cascade_far_distances,
		const SkyAmbientLighting&
			sky_ambient)
	{
		updateImpl(
			shadow_view_projections,
			cascade_far_distances,
			static_cast<std::uint32_t>(
				shadow_view_projections.size()),
			m_DirectionalLights.empty() ? 0u : 1u,
			sky_ambient);
	}

	void LightContainer::updateDirectionalShadows(
		std::span<const glm::mat4>
			shadow_view_projections,
		std::span<const float>
			cascade_far_distances,
		std::uint32_t cascade_count,
		std::uint32_t shadow_light_count,
		const SkyAmbientLighting&
			sky_ambient)
	{
		updateImpl(
			shadow_view_projections,
			cascade_far_distances,
			cascade_count,
			shadow_light_count,
			sky_ambient);
	}

	void LightContainer::ensureDirectionalShadowBufferCapacity(
		vk::DeviceSize required_bytes)
	{
		if (required_bytes <=
			m_DirectionalShadowBufferCapacity)
		{
			return;
		}
		auto replacement_capacity =
			m_DirectionalShadowBufferCapacity;
		while (replacement_capacity < required_bytes)
		{
			if (replacement_capacity >
				std::numeric_limits<vk::DeviceSize>::max() / 2)
			{
				throw std::overflow_error(
					"directional shadow buffer capacity overflow");
			}
			replacement_capacity *= 2;
		}
		auto replacement =
			GET_MODULE(VulkanManageCore).allocBuf(
				replacement_capacity,
				vk::BufferUsageFlagBits::eStorageBuffer,
				vma::MemoryUsage::eAuto,
				vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
		auto retired =
			std::move(m_DirectionalShadowBuffer);
		m_DirectionalShadowBuffer =
			std::move(replacement);
		m_DirectionalShadowBufferCapacity =
			replacement_capacity;
		if (auto* queue =
			FastModuleContainer::tryGet<DeletionQueue>();
			queue != nullptr &&
			queue->acceptingResources())
		{
			queue->defer(std::move(retired));
		}
		else
		{
			GET_MODULE(VulkanManageCore).waitIdle();
		}
	}

	void LightContainer::updateImpl(
		std::span<const glm::mat4>
			shadow_view_projections,
		std::span<const float>
			cascade_far_distances,
		std::uint32_t cascade_count,
		std::uint32_t shadow_light_count,
		const SkyAmbientLighting&
			sky_ambient)
	{
		if (cascade_count == 0 ||
			cascade_count >
				maximumDirectionalShadowCascades)
		{
			throw std::runtime_error(
				"directional shadow cascade count is outside the LightUBO ABI");
		}
		if (shadow_light_count >
			m_DirectionalLights.size())
		{
			throw std::runtime_error(
				"directional shadow light count exceeds the loaded directional inventory");
		}
		const auto rendered_light_slots =
			std::max(1u, shadow_light_count);
		const auto projection_count =
			static_cast<std::uint64_t>(
				rendered_light_slots) *
			cascade_count;
		if (projection_count !=
			shadow_view_projections.size())
		{
			throw std::runtime_error(
				"directional shadow projection count does not match light and cascade counts");
		}
		if (!cascade_far_distances.empty() &&
			cascade_far_distances.size() !=
				cascade_count)
		{
			throw std::runtime_error(
				"directional shadow split count does not match the projection count");
		}
		float prior_far = 0.0f;
		for (const auto far_distance :
			cascade_far_distances)
		{
			if (!std::isfinite(far_distance) ||
				far_distance <= prior_far)
			{
				throw std::runtime_error(
					"directional shadow cascade far distances must be finite and strictly increasing");
			}
			prior_far = far_distance;
		}

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
		ubo.directionalShadowCascadeCount =
			cascade_count;
		for (auto &split :
			ubo.directionalShadowCascadeSplits)
		{
			split =
				glm::vec4{
					std::numeric_limits<float>::max()};
		}
		for (auto &projection :
			ubo.shadowViewProjections)
		{
			projection = glm::mat4{1.0f};
		}
		for (std::size_t index = 0;
			index < cascade_count;
			++index)
		{
			ubo.shadowViewProjections[index] =
				shadow_view_projections[index];
			if (!cascade_far_distances.empty())
			{
				ubo.directionalShadowCascadeSplits[
					index / 4][index % 4] =
					cascade_far_distances[index];
			}
		}
		ubo.environmentAmbientRadiance =
			glm::vec4{
				sky_ambient.color *
					sky_ambient.ambient_intensity,
				0.0f};
		ubo.environmentSkyRadiance =
			glm::vec4{
				sky_ambient.color *
					sky_ambient.sky_intensity,
				0.0f};

		auto shadow_inventory_indices =
			std::vector<std::uint32_t>(
				shadow_light_count);
		std::iota(
			shadow_inventory_indices.begin(),
			shadow_inventory_indices.end(), 0u);
		const auto shadow_matrix_count =
			static_cast<std::size_t>(
				shadow_light_count) *
			cascade_count;
		const auto packed_shadow_data =
			packDirectionalShadowDataV1(
				shadow_inventory_indices,
				cascade_count,
				shadow_view_projections.first(
					shadow_matrix_count));
		const auto shadow_bytes =
			static_cast<vk::DeviceSize>(
				packed_shadow_data.elements.size() *
				sizeof(glm::uvec4));
		ensureDirectionalShadowBufferCapacity(
			shadow_bytes);
		m_DirectionalShadowDataElementCount =
			packed_shadow_data.elements.size();

		auto& vkcore = GET_MODULE(VulkanManageCore);
		vkcore.writeBuf(
			m_LightUBO, &ubo, 0, sizeof(ubo));
		vkcore.writeBuf(
			m_DirectionalShadowBuffer,
			packed_shadow_data.elements.data(),
			0, shadow_bytes);
	}
}
