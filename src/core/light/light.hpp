#pragma once

#include <glm/glm.hpp>
#include <string>

namespace Pelican
{
	constexpr uint32_t MAX_DIRECTIONAL_LIGHTS = 8;
	constexpr uint32_t MAX_POINT_LIGHTS = 16;
	constexpr uint32_t MAX_SPOT_LIGHTS = 8;

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

	struct SpotLight_UBO
	{
		glm::vec3 position;
		float innerConeAngle; // cos(angle)
		glm::vec3 direction;
		float outerConeAngle; // cos(angle)
		glm::vec3 color;
		float intensity;
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

	struct SpotLight
	{
		std::string name;
		glm::vec3 position;
		glm::vec3 direction;
		float intensity;
		float innerConeAngle; // degrees
		float outerConeAngle; // degrees
		glm::vec3 color;
	};

	struct LightUBO
	{
		uint32_t directionalLightCount;
		uint32_t pointLightCount;
		uint32_t spotLightCount;
		float padding; // for alignment
		DirectionalLight_UBO directionalLights[MAX_DIRECTIONAL_LIGHTS];
		PointLight_UBO pointLights[MAX_POINT_LIGHTS];
		SpotLight_UBO spotLights[MAX_SPOT_LIGHTS];
	};
}
