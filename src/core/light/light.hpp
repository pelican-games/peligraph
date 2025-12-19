#pragma once

#include <glm/glm.hpp>
#include <string>

namespace Pelican
{
	constexpr uint32_t MAX_DIRECTIONAL_LIGHTS = 4;
	constexpr uint32_t MAX_POINT_LIGHTS = 4;

	struct DirectionalLight_UBO
	{
		glm::vec3 direction;
		float intensity;
		glm::vec3 color;
		float padding; // for alignment
	};

	struct PointLight_UBO
	{
		glm::vec3 position;
		float intensity;
		glm::vec3 color;
		float padding; // for alignment
	};

	struct DirectionalLight
	{
		std::string name;
		glm::vec3 direction;
		float intensity;
		glm::vec3 color;
	};

	struct PointLight
	{
		std::string name;
		glm::vec3 position;
		float intensity;
		glm::vec3 color;
	};

	struct LightUBO
	{
		uint32_t directionalLightCount;
		uint32_t pointLightCount;
		glm::vec2 padding; // for alignment
		DirectionalLight_UBO directionalLights[MAX_DIRECTIONAL_LIGHTS];
		PointLight_UBO pointLights[MAX_POINT_LIGHTS];
	};
}
