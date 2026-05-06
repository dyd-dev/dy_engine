#pragma once
#include <cstdint>
#include "Math/Math.h"

/* ShadowMap.h
 *
 * Directional Light용 Shadow Map의 Light-Space 행렬 계산을 담당.
 *
 * 본 엔진은 두 종류의 그림자가 공존:
 *   - Planar Projected Shadow (Graphics/Shadow.h): CPU에서 평면 투영 메시 생성
 *   - Shadow Map (이 파일): GPU 깊이 텍스처 기반, 임의 형상 receiver 지원
 *
 * 호출 규약:
 *   Math::float4x4 lvp = ComputeDirectionalLightViewProj(lightDir, desc);
 *   commandList->BindConstantBuffer(shadowMatrixBinding, shadowMatrixBuffer, 0, sizeof(lvp.m));
 *
 * 좌표 가정:
 *   - Z-up 월드 좌표계 (본 예제와 동일)
 *   - Vulkan NDC: x∈[-1,1], y∈[-1,1], z∈[0,1]
 *   - column-major 4x4
 */

namespace dy::Graphics
{
	struct ShadowMapDesc
	{
		uint32_t resolution     = 2048;             // Shadow Map 해상도 (정사각형)
		float    orthoWidth     = 6.0f;             // 라이트 frustum 폭(월드 단위)
		float    orthoHeight    = 6.0f;             // 라이트 frustum 높이
		float    nearPlane      = 0.1f;
		float    farPlane       = 20.0f;
		Math::float3 sceneCenter = Math::float3(0.0f, 0.0f, 0.0f); // 라이트가 노려보는 점
		float    lightDistance  = 8.0f;             // 광원을 sceneCenter 위로 얼마나 띄울지
	};

	// Directional Light용 Light-Space ViewProjection 행렬 계산.
	// 광원 위치 = sceneCenter - normalize(lightDirection) * lightDistance
	// 광원이 sceneCenter를 바라보는 LookAt + Orthographic 합성.
	[[nodiscard]] Math::float4x4 ComputeDirectionalLightViewProj(
		const Math::float3& lightDirection,
		const ShadowMapDesc& desc);
}
