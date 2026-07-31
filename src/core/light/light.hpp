#pragma once

#include "../../project/viewfamilyrelation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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

	struct SkyAmbientLighting
	{
		glm::vec3 color{0.0f};
		float ambient_intensity = 0.0f;
		float sky_intensity = 0.0f;
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
		alignas(16) uint32_t directionalShadowCascadeCount;
		uint32_t directionalShadowPadding0;
		uint32_t directionalShadowPadding1;
		uint32_t directionalShadowPadding2;
		std::array<glm::vec4, 2> directionalShadowCascadeSplits;
		std::array<
			glm::mat4,
			maximumDirectionalShadowCascades>
			shadowViewProjections;
		glm::vec4 environmentAmbientRadiance;
		glm::vec4 environmentSkyRadiance;
	};

	static_assert(
		offsetof(
			LightUBO,
			directionalShadowCascadeCount) %
				16 ==
			0);
	static_assert(
		offsetof(
			LightUBO,
			directionalShadowCascadeSplits) ==
		offsetof(
			LightUBO,
			directionalShadowCascadeCount) +
			16);
	static_assert(
		offsetof(
			LightUBO,
			shadowViewProjections) ==
		offsetof(
			LightUBO,
			directionalShadowCascadeSplits) +
			sizeof(glm::vec4) * 2);
	static_assert(
		offsetof(
			LightUBO,
			environmentAmbientRadiance) ==
		offsetof(
			LightUBO,
			shadowViewProjections) +
			sizeof(glm::mat4) *
				maximumDirectionalShadowCascades);
	static_assert(
		offsetof(
			LightUBO,
			environmentSkyRadiance) ==
		offsetof(
			LightUBO,
			environmentAmbientRadiance) +
			sizeof(glm::vec4));
	static_assert(sizeof(LightUBO) % 16 == 0);

	struct DirectionalShadowView
	{
		glm::mat4 view{1.0f};
		glm::mat4 projection{1.0f};
		glm::vec3 camera_position{0.0f};
	};
}
