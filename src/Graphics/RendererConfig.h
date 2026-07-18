#pragma once
#include <cstdint>

#include "Math/Math.h"
#include "RHI/Format.h"
#include "RHI/ShaderLayout.h"
#include "Graphics/Mesh.h"
#include "Graphics/ShadowMath.h" // ShadowMapDesc

namespace dy::Graphics
{
	enum class RendererBindingMode : uint32_t
	{
		PerDrawBind = 0,
		BatchedBind = 1,
		Bindless = 2
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

	// 고수준 카메라 명세. Renderer::SetCamera 가 view·proj·cameraPosition 을 만들고
	// 백엔드 클립공간 Y 뒤집기까지 처리한다. 사용자는 백엔드 NDC 관례를 몰라도 된다.
	// (직접 행렬을 다루려는 deep 유저는 SetViewProjection/SetCameraPosition 으로 우회.)
	struct CameraDesc
	{
		Math::float3 eye = Math::float3(0.0f, 0.0f, 3.0f);
		Math::float3 target = Math::float3(0.0f, 0.0f, 0.0f);
		Math::float3 up = Math::float3(0.0f, 0.0f, 1.0f);
		float fovYRadians = 1.0472f;   // 원근 투영용(약 60도)
		float aspect = 16.0f / 9.0f;
		float nearPlane = 0.1f;
		float farPlane = 100.0f;
		bool orthographic = false;     // true 면 ortho* 사용
		float orthoWidth = 0.0f;
		float orthoHeight = 0.0f;
	};

	struct RendererDesc
	{
		RendererBindingMode bindingMode = RendererBindingMode::PerDrawBind;
		const char* vertexShaderPath = nullptr;
		const char* pixelShaderPath = nullptr;
		const char* shadowVertexShaderPath = nullptr;
		const char* toneMapVertexShaderPath = nullptr;
		const char* toneMapPixelShaderPath = nullptr;
		RHI::Format renderTargetFormat = RHI::Format::R8G8B8A8_UNORM;
		RHI::Format depthStencilFormat = RHI::Format::D32_FLOAT;
		Math::float4 clearColor = Math::float4(0.08f, 0.10f, 0.14f, 1.0f);
		Math::float4x4 viewProjectionMatrix = Math::float4x4::Identity();
		Math::float4x4 cameraViewMatrix = Math::float4x4::Identity();
		Math::float3 cameraPosition = Math::float3(0.0f, 0.0f, 2.2f);
		Math::float3 cameraForward = Math::float3(0.0f, 1.0f, 0.0f);
		Math::float3 cameraUp = Math::float3(0.0f, 0.0f, 1.0f);
		float cameraNearPlane = 0.1f;
		float cameraFarPlane = 100.0f;
		float cameraFovYRadians = 1.04719755f;
		float cameraAspect = 16.0f / 9.0f;
		Math::float3 directionalLightDirection = Math::float3(0.35f, 0.65f, 0.68f);
		Math::float3 directionalLightColor = Math::float3(1.0f, 0.94f, 0.82f);
		float directionalLightIntensity = 4.0f;
		Math::float3 ambientColor = Math::float3(1.0f, 1.0f, 1.0f);
		float ambientIntensity = 0.035f;
		PBRDesc pbr = {};
		EnvironmentDesc environment = {};
		bool enableHdrRendering = false;
		float exposure = 1.0f;
		bool enableShadows = false;
		bool enableMainPass = true;
		bool enableBindlessTextures = false;
		float shadowStrength = 0.45f;
		ShadowMapDesc shadowMap = {};
		bool autoFitShadowMap = true;
		float shadowBoundsPadding = 0.25f;
		float shadowDepthBias = 0.0007f;
		float shadowSlopeBias = 0.003f;
		float shadowNormalBias = 0.0f;
		uint32_t shadowPcfRadius = 1;
		uint32_t shadowCascadeCount = 4u;
		float shadowCascadeSplitLambda = 0.65f;
		float shadowLightRadius = 0.05f;
		float shadowBlockerSearchRadius = 2.0f;
		float shadowMaxFilterRadius = 8.0f;

		// 셰이더 바인딩 ABI(데이터). 미지정 시 엔진 표준 계약. 사용자 셰이더 제공 시 함께 지정.
		RHI::ShaderLayoutDesc shaderLayout = {};
	};
}
