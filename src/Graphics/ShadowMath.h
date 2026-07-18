#pragma once
#include <array>
#include <cstdint>

#include "Core/Types.h"
#include "Graphics/Mesh.h" // MeshData
#include "Math/Math.h"

// 그림자/광원 시점(view-projection) 수학. 메시 데이터와 별개의 관심사라 Mesh 에서 분리.
// 광원은 데이터(struct)로 유지하고, 시점 행렬 계산은 여기(자유 함수)에 모은다.
namespace dy::Graphics
{
	inline constexpr uint32_t kMaxShadowCascades = 4u;
	inline constexpr uint32_t kMaxShadowViews = 6u;

	// 평면 투영 그림자(레거시) 파라미터.
	struct ShadowDesc
	{
		Math::float3 lightDirection = Math::float3(0.0f, 0.0f, 1.0f);
		float receiverPlaneZ = 0.0f;
		float bias = 0.001f;
	};

	// 그림자 맵(광원 시점 카메라) 파라미터.
	struct ShadowMapDesc
	{
		uint32_t resolution = 2048;
		float orthoWidth = 6.0f;
		float orthoHeight = 6.0f;
		float nearPlane = 0.1f;
		float farPlane = 20.0f;
		float spotFovYRadians = 1.57079633f;
		Math::float3 sceneCenter = Math::float3(0.0f, 0.0f, 0.0f);
		float lightDistance = 8.0f;
	};

	struct CameraFrustumDesc
	{
		Math::float3 position = Math::float3(0.0f, 0.0f, 0.0f);
		Math::float3 forward = Math::float3(0.0f, 1.0f, 0.0f);
		Math::float3 up = Math::float3(0.0f, 0.0f, 1.0f);
		float nearPlane = 0.1f;
		float farPlane = 100.0f;
		float fovYRadians = 1.04719755f;
		float aspect = 16.0f / 9.0f;
	};

	struct DirectionalCascadeData
	{
		std::array<Math::float4x4, kMaxShadowCascades> viewProjections = {};
		std::array<float, kMaxShadowCascades> splits = {};
		uint32_t count = 0u;
	};

	[[nodiscard]] std::array<float, kMaxShadowCascades> ComputePracticalCascadeSplits(
		float nearPlane,
		float farPlane,
		uint32_t cascadeCount,
		float lambda);

	[[nodiscard]] DirectionalCascadeData ComputeDirectionalCascades(
		const CameraFrustumDesc& camera,
		const Math::float3& lightDirection,
		const ShadowMapDesc& baseDesc,
		uint32_t cascadeCount,
		float splitLambda,
		float boundsPadding);

	[[nodiscard]] std::array<Math::float4x4, kMaxShadowViews> ComputePointLightViewProjections(
		const Math::float3& lightPosition,
		float nearPlane,
		float farPlane);

	[[nodiscard]] Math::float4x4 ComputeDirectionalLightViewProj(
		const Math::float3& lightDirection,
		const ShadowMapDesc& desc);

	[[nodiscard]] Math::float4x4 ComputeSpotLightViewProj(
		const Math::float3& lightPosition,
		const Math::float3& lightDirection,
		const ShadowMapDesc& desc);

	[[nodiscard]] ShadowMapDesc FitDirectionalShadowMapToBounds(
		const Math::float3& lightDirection,
		const ShadowMapDesc& baseDesc,
		const Math::float3& boundsMin,
		const Math::float3& boundsMax,
		float padding);

	[[nodiscard]] MeshData BuildShadowMesh(const MeshData& sourceMesh, const Math::float4x4& worldMatrix, const ShadowDesc& desc);
}
