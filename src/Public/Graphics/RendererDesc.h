#pragma once

#include <cstdint>

#include "Math/Math.h"
#include "Graphics/Shaders.h"

namespace dy::Graphics
{
	enum class ColorFormat : uint8_t
	{
		RGBA8Unorm,
		BGRA8Unorm,
		RGBA8UnormSrgb,
		BGRA8UnormSrgb
	};

	enum class DepthFormat : uint8_t
	{
		None,
		D32Float,
		D24UnormStencil8
	};

	struct PBRDesc
	{
		float minRoughness = 0.04f;
		float ambientSpecularStrength = 0.25f;
	};

	struct EnvironmentDesc
	{
		Math::float3 diffuseColor = Math::float3(1.0f, 1.0f, 1.0f);
		float diffuseIntensity = 1.0f;
		Math::float3 specularColor = Math::float3(1.0f, 1.0f, 1.0f);
		float specularIntensity = 1.0f;
	};

	struct ShadowMapDesc
	{
		uint32_t resolution = 2048;
		float orthoWidth = 6.0f;
		float orthoHeight = 6.0f;
		float nearPlane = 0.1f;
		float farPlane = 20.0f;
		Math::float3 sceneCenter = Math::float3(0.0f, 0.0f, 0.0f);
		float lightDistance = 8.0f;
	};

	struct RendererDesc
	{
		// Empty assets select the built-in program. Custom assets must implement
		// Graphics/ShaderLayout.h; bytecode must outlive the renderer.
		ShaderAssets meshShaders = {};
		ShaderAssets canvasShaders = {};
		ColorFormat outputFormat = ColorFormat::BGRA8Unorm;
		uint32_t backBufferCount = 2;
		bool vsync = true;
		bool allowReadback = false;
		DepthFormat depthStencilFormat = DepthFormat::D32Float;
		DepthFormat shadowFormat = DepthFormat::D32Float;
		Math::float4 clearColor = Math::float4(0.08f, 0.10f, 0.14f, 1.0f);
		Math::float3 ambientColor = Math::float3(1.0f, 1.0f, 1.0f);
		float ambientIntensity = 0.035f;
		PBRDesc pbr = {};
		EnvironmentDesc environment = {};
		bool enableShadows = false;
		ShadowMapDesc shadowMap = {};
		bool autoFitShadowMap = true;
		float shadowBoundsPadding = 0.25f;
		float shadowDepthBias = 0.0007f;
		float shadowSlopeBias = 0.003f;
		float shadowNormalBias = 0.0f;
		float shadowRasterSlopeBias = 1.75f;
		uint32_t shadowPcfRadius = 1;
	};
}
