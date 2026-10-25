#include "RendererInternal.h"
#include "SceneAlgorithms.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/Format.h"
#include "dyf/RHI/ICommandList.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

using namespace dyf;

namespace
{
	// 방향 길이를 1로 맞추되 너무 짧은 입력은 기본 -Z로 대체해 광원 계산의 기준 축을 유지한다.
	[[nodiscard]] inline Math::float3 NormalizeDirection(const Math::float3& direction)
	{
		return Math::NormalizeOr(direction, Math::float3(0.0f, 0.0f, -1.0f));
	}

	// 두 벡터의 대응 성분을 곱해 더하는 연산(내적)으로 up의 direction 성분을 구해 뺀다. 호출부에서 direction을 정규화했으므로 결과는 direction과 수직이다.
	// 두 입력이 평행해 결과가 너무 짧으면 아래 보조축으로 다시 계산한다.
	[[nodiscard]] inline Math::float3 OrthogonalUp(const Math::float3& direction, const Math::float3& up)
	{
		const Math::float3 projected = up - direction * Math::Dot(up, direction);
		// direction의 Z 성분 절댓값이 0.99 이상이면 Y축을, 아니면 Z축을 보조축으로 고른다. 이 축에서도 direction 성분을 빼 정규화하여 평행한 up 입력을 대체한다.
		const Math::float3 fallback = std::fabs(direction.z) < 0.99f
			? Math::float3(0.0f, 0.0f, 1.0f)
			: Math::float3(0.0f, 1.0f, 0.0f);
		return Math::NormalizeOr(projected, Math::NormalizeOr(
			fallback - direction * Math::Dot(fallback, direction),
			Math::float3(1.0f, 0.0f, 0.0f)));
	}
}

// 상수 버퍼를 준비하고 광원 우선순위 선택 → 고정 배열 변환 → 변경 바이트 비교 → 필요한 업로드 순서로 진행한다.
bool Renderer::Impl::UpdateLightingBuffer(
	const Scene& scene,
	const Camera& camera,
	RHI::IDevice* device,
	RHI::ICommandList& commandList)
{
	if(lightingBuffer && lightingBuffer->GetDesc().size!=sizeof(RendererLightingConstants)) {lightingBuffer=nullptr;lightingReady=false;} // 상수 크기가 달라졌으면 핸들을 비워 새 버퍼를 만들고 lightingReady를 false로 돌린다. 이전 버퍼의 해제 또는 실패 복구는 호출부의 FrameBuffers가 맡는다.
	if(!lightingBuffer) lightingBuffer = device->CreateBuffer(RHI::BufferDesc{ // 핸들이 없으면 CopyDestination 상태의 조명 상수 버퍼를 만든다.
		static_cast<uint32_t>(sizeof(RendererLightingConstants)),
		static_cast<uint32_t>(sizeof(RendererLightingConstants)),
		RHI::BufferUsage::Constant,
		RHI::ResourceState::CopyDestination
	});
	if(lightingBuffer == nullptr) return false; // 버퍼 생성 실패를 상위 프레임 준비 단계로 전달한다.

    // 종류별 scratch 용량을 재사용하고 priority 내림차순·동점 원본 인덱스 오름차순으로 정렬해 stable_sort 임시 메모리 없이 선택 순서를 고정한다.
    const auto selectIndices=[&](const auto& lights,uint32_t capacity,uint32_t slot)->const std::vector<uint32_t>& {
        auto& indices=lightingIndexScratch[slot];indices.clear();
        if(!config.lighting.direct) return indices; // 직접 조명을 끈 경우 그 종류의 선택 목록을 빈 상태로 반환한다.
        indices.reserve(lights.size());
        for(uint32_t i=0;i<lights.size();++i)if(lights[i].enabled)indices.push_back(i); // enabled인 원본 인덱스만 정렬 후보로 넣는다.
        std::sort(indices.begin(),indices.end(),[&](uint32_t a,uint32_t b) {
            return lights[a].priority==lights[b].priority ? a<b : lights[a].priority>lights[b].priority;
        });
        if(indices.size()>capacity)indices.resize(capacity); // 셰이더 고정 배열 길이를 넘는 낮은 우선순위 후보를 제외한다.
        return indices;
    };
	// 구조체 전체를 0으로 시작해 쓰지 않는 광원 슬롯도 결정된 값이 되게 한다. 바이트 캐시 비교의 기준도 이 완성 구조체다.
	RendererLightingConstants constants = {};
	const auto& directionalIndices=selectIndices(scene.DirectionalLights(),static_cast<uint32_t>(std::size(constants.directionalLights)),0);
	const auto& pointIndices=selectIndices(scene.PointLights(),static_cast<uint32_t>(std::size(constants.pointLights)),1);
	// 아래 단일 광원 필드는 선택된 첫 방향광/점광원으로 채운다. 이어지는 다중 광원 배열과 함께 유지되는 셰이더 레이아웃이다.
	const DirectionalLight* light = directionalIndices.empty() ? nullptr : &scene.DirectionalLights()[directionalIndices.front()];
	const PointLight* pointLight = pointIndices.empty() ? nullptr : &scene.PointLights()[pointIndices.front()];
	const Math::float3 lightDirection = light != nullptr
		? (Math::LengthSquared(light->direction)<=1e-8f ? Math::float3(0,0,-1) : light->direction)
		: Math::float3(0.0f, 0.0f, 1.0f);
	const Math::float3 lightColor = light != nullptr
		? light->color
		: Math::float3(0.0f, 0.0f, 0.0f);
	const float lightIntensity = light != nullptr ? light->intensity : 0.0f;
	// 여기서는 기존 단일 방향광 필드의 그림자 표시를 만든다. 실제 뷰 수·종류별 그림자 배치는 별도의 ShadowData 상수가 담는다.
	const bool shadowsEnabled = shadowPipeline != nullptr &&
		light != nullptr && light->castShadow;
	const float shadowStrength = shadowsEnabled ? light->shadowStrength : 0.0f;

	// float4의 xyz는 위치 또는 방향이고 w에는 그림자 강도·활성 여부 같은 별도 의미를 함께 담는다. 좌표의 동차 w로 해석하면 안 된다.
	constants.cameraPosition = Math::float4(
		camera.position.x,
		camera.position.y,
		camera.position.z,
		shadowsEnabled ? shadowStrength : 0.0f);
	constants.directionalLightDirection = Math::float4(
		lightDirection.x,
		lightDirection.y,
		lightDirection.z,
		shadowsEnabled ? 1.0f : 0.0f);
	constants.directionalLightColor = Math::float4(lightColor.x, lightColor.y, lightColor.z, lightIntensity);
	// 주변광과 환경 diffuse의 색·강도를 곱하고 diffuse 토글을 강도에 반영한다.
	constants.ambientColor = Math::float4(
		config.lighting.ambientColor.x * config.lighting.environment.diffuseColor.x,
		config.lighting.ambientColor.y * config.lighting.environment.diffuseColor.y,
		config.lighting.ambientColor.z * config.lighting.environment.diffuseColor.z,
		config.lighting.ambientIntensity * config.lighting.environment.diffuseIntensity * (config.lighting.environment.diffuse ? 1.0f : 0.0f));
	constants.shadowParams = Math::float4(
		0.0007f,
		0.003f,
		0.0f,
		config.lighting.shadowQuality == ShadowQuality::High ? 2.0f : 1.0f);
	// 일반 조명 출력에서 pbrParams.z=-1이면 선형 HDR을 남겨 후단 출력 패스에 맡긴다. 0이면 직접 출력에 노출·톤 매핑만 적용하고 sRGB 저장 변환을 쓰며, 1이면 셰이더의 감마 변환도 적용한다. w는 노출이다.
	constants.pbrParams = Math::float4(
		0.04f,
		0.25f,
		config.enableHdrRendering ? -1.0f : (RHI::IsSrgbFormat(meshColorFormat) ? 0.0f : 1.0f),
		config.exposure);
	constants.environmentColor = Math::float4(
		config.lighting.environment.specularColor.x,
		config.lighting.environment.specularColor.y,
		config.lighting.environment.specularColor.z,
		config.lighting.environment.specularIntensity * (config.lighting.environment.specular ? 1.0f : 0.0f));
	if(pointLight != nullptr) // 선택된 첫 점광원이 있을 때만 단일 광원 필드를 채우며 없으면 초기 0을 유지한다.
	{
		constants.pointLightPositionRange = Math::float4(
			pointLight->position.x, pointLight->position.y, pointLight->position.z, pointLight->range);
		constants.pointLightColorIntensity = Math::float4(
			pointLight->color.x, pointLight->color.y, pointLight->color.z, pointLight->intensity);
	}

    // 기본 셰이더의 고정 배열 크기에 맞춰 활성 광원을 선택하고 GPU 형식으로 변환한다.
        const auto& spotIndices=selectIndices(scene.SpotLights(),static_cast<uint32_t>(std::size(constants.spotLights)),2);
        const auto& rectAreaIndices=selectIndices(scene.RectAreaLights(),static_cast<uint32_t>(std::size(constants.rectAreaLights)),3);
        const auto& discAreaIndices=selectIndices(scene.DiscAreaLights(),static_cast<uint32_t>(std::size(constants.discAreaLights)),4);
		// 방향광 배열은 정규화 방향과 0 이상 강도를 저장한다. 선택한 인덱스 순서가 그림자 고정 슬롯과 맞아야 한다.
		const uint32_t directionalCount = static_cast<uint32_t>(directionalIndices.size());
		for(uint32_t index = 0u; index < directionalCount; ++index)
		{
			const DirectionalLight& light = scene.DirectionalLights()[directionalIndices[index]];
			const Math::float3 direction = NormalizeDirection(light.direction);
			constants.directionalLights[index] = {
				Math::float4(direction.x, direction.y, direction.z, std::max(light.intensity, 0.0f)),
				Math::float4(light.color.x, light.color.y, light.color.z, 0.0f)
			};
		}

		// 점광원의 위치/범위와 색/강도를 두 float4에 담고 범위·강도는 0 이상으로 제한한다. LocalLightData가 양의 범위·강도를 가진 활성 점광원·스폿 전체를 담는 것과 달리, 이 배열은 우선순위로 선택한 점광원만 담는다.
		const uint32_t pointCount = static_cast<uint32_t>(pointIndices.size());
		for(uint32_t index = 0u; index < pointCount; ++index)
		{
			const PointLight& light = scene.PointLights()[pointIndices[index]];
			constants.pointLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.range, 0.0f)),
				Math::float4(light.color.x, light.color.y, light.color.z, std::max(light.intensity, 0.0f))
			};
		}

		// 스폿은 위치·방향 외에 원뿔 경계 cos를 저장한다. 셰이더는 각도를 다시 구하지 않고 내적으로 안팎을 판단한다.
		const uint32_t spotCount = static_cast<uint32_t>(spotIndices.size());
		for(uint32_t index = 0u; index < spotCount; ++index)
		{
			const SpotLight& light = scene.SpotLights()[spotIndices[index]];
			// EffectiveSpotCone은 비유한 반각을 0으로 바꾼 뒤 작은 값/큰 값 순서로 정렬하고 0..89도로 제한한다. 같은 보정 결과를 그림자 투영에도 사용한다.
			const auto cone=EffectiveSpotCone(light.innerConeRadians,light.outerConeRadians);
			const Math::float3 direction = NormalizeDirection(light.direction);
			constants.spotLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.range, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, std::cos(cone.outer)),
				Math::float4(light.color.x, light.color.y, light.color.z, std::max(light.intensity, 0.0f)),
				Math::float4(std::cos(cone.inner), 0.0f, 0.0f, 0.0f)
			};
		}

		// 사각 면광원의 방향·직교 up·폭·높이를 묶어 셰이더가 광원 평면을 복원하게 한다.
		const uint32_t rectAreaCount = static_cast<uint32_t>(rectAreaIndices.size());
		for(uint32_t index = 0u; index < rectAreaCount; ++index)
		{
			const RectAreaLight& light = scene.RectAreaLights()[rectAreaIndices[index]];
			const Math::float3 direction = NormalizeDirection(light.direction);
			const Math::float3 up = OrthogonalUp(direction, light.up);
			constants.rectAreaLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.intensity, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, light.width),
				Math::float4(up.x, up.y, up.z, light.height),
				Math::float4(light.color.x, light.color.y, light.color.z, 0.0f)
			};
		}

		const uint32_t discAreaCount = static_cast<uint32_t>(discAreaIndices.size());
		if(shaderSources.bytes[MeshFragment].empty() && config.lighting.enabled && (rectAreaCount || discAreaCount) && // 기본 셰이더로 면광원을 평가하려는데 LTC 조회 텍스처가 없으면 모듈 누락을 오류로 알린다.
			(!frameLighting.areaMatrix.texture || !frameLighting.areaAmplitude.texture))
		{
			std::fputs("dyf: LTC area lights require the ClusteredLighting rendering module.\n",stderr);
			return false;
		}
		// 원판 면광원도 평면 방향과 up을 정리하되 크기는 폭·높이 대신 반지름 하나를 사용한다.
		for(uint32_t index = 0u; index < discAreaCount; ++index)
		{
			const DiscAreaLight& light = scene.DiscAreaLights()[discAreaIndices[index]];
			const Math::float3 direction = NormalizeDirection(light.direction);
			const Math::float3 up = OrthogonalUp(direction, light.up);
			constants.discAreaLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.intensity, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, light.radius),
				Math::float4(up.x, up.y, up.z, 0.0f),
				Math::float4(light.color.x, light.color.y, light.color.z, 0.0f)
			};
		}

		// 각 고정 배열에 선택해 넣은 광원 수와 조명 활성 비트를 기록한다. 셰이더는 그 개수만 순회하므로 남은 0 초기화 슬롯을 평가하지 않는다.
		constants.lightCounts = Math::float4(static_cast<float>(directionalCount), static_cast<float>(pointCount), static_cast<float>(spotCount), config.lighting.enabled ? 1.0f : 0.0f);
		constants.areaLightCounts = Math::float4(static_cast<float>(rectAreaCount), static_cast<float>(discAreaCount), 0.0f, 0.0f);

        if(!config.lighting.enabled) { constants.lightCounts = {}; constants.areaLightCounts = {}; } // 조명 전체가 꺼지면 광원 개수를 0으로 만들어 평가를 생략한다. 재질 기본색·발광 처리는 셰이더에 남는다.

        constants.shadowLight = {1,0,constants.cameraPosition.w,constants.directionalLightDirection.w};

    const auto* first=reinterpret_cast<const uint8_t*>(&constants);
    // 이번 상수의 CPU 바이트를 임시 캐시에 복사한다. 여기서 lightingBytes를 교체하지 않아 제출 실패가 기존 성공 캐시를 덮지 않는다.
    pendingLightingBytes.assign(first,first+sizeof(constants));
    const bool noAreaLights=constants.areaLightCounts.x==0.0f && constants.areaLightCounts.y==0.0f;
    if(lightingReady && pendingLightingBytes==lightingBytes) {frameNoAreaLights=noAreaLights;return true;} // 직전 성공 업로드와 값이 완전히 같으면 기존 상수 버퍼를 그대로 읽는다.
	if(lightingReady) { const RHI::ResourceBarrierDesc barrier={lightingBuffer,nullptr,RHI::ResourceState::ConstantBuffer,RHI::ResourceState::CopyDestination,{}}; commandList.ResourceBarrier(&barrier,1); } // 기존 상수 버퍼를 다시 갱신할 때만 복사 대상 상태로 전환한다.
	if(!device->UpdateBuffer( // CPU 상수를 GPU 버퍼에 복사하는 명령 기록에 실패하면 false를 반환한다.
			commandList,
			lightingBuffer,
			0,
			&constants,
			static_cast<uint32_t>(sizeof(constants))))
		return false;
	// 복사 기록 뒤 ConstantBuffer 상태로 돌아가게 한다. 상태 전환 기록과 실제 GPU 완료는 별도 단계다.
	const RHI::ResourceBarrierDesc after = {
		lightingBuffer, nullptr,
		RHI::ResourceState::CopyDestination,
		RHI::ResourceState::ConstantBuffer,
		{}
	};
	commandList.ResourceBarrier(&after, 1);
	frameNoAreaLights=noAreaLights;
	return true;
}

// BuildShadows가 만든 모든 광원 행렬·아틀라스 위치·필터 매개변수를 한 상수 버퍼에 올린다.
bool Renderer::Impl::UpdateShadowBuffer(
	RHI::IDevice* device,
	RHI::ICommandList& commandList,const ShadowData& shadows)
{
	if(!config.lighting.enabled || !config.lighting.direct || !config.lighting.shadows) return true; // 조명·직접광·그림자 중 하나라도 비활성이면 업로드가 필요 없는 정상 성공으로 처리한다.
	if(shadowMatrixBuffer && shadowMatrixBuffer->GetDesc().size!=sizeof(RendererShadowConstants)) {shadowMatrixBuffer=nullptr;shadowReady=false;} // 그림자 상수 크기가 달라졌으면 핸들을 비워 재생성하고 shadowReady를 false로 돌린다. 이전 버퍼의 해제 또는 실패 복구는 호출부의 FrameBuffers가 맡는다.
	if(!shadowMatrixBuffer) shadowMatrixBuffer = device->CreateBuffer(RHI::BufferDesc{ // 핸들이 비어 있으면 현재 RendererShadowConstants 크기의 버퍼를 CopyDestination 상태로 만든다.
		static_cast<uint32_t>(sizeof(RendererShadowConstants)),
		static_cast<uint32_t>(sizeof(RendererShadowConstants)),
		RHI::BufferUsage::Constant,
		RHI::ResourceState::CopyDestination
	});
	if(shadowMatrixBuffer == nullptr) return false; // 할당 실패 시 유효한 그림자 상수처럼 진행하지 않는다.

    const auto& shadow=shadows.constants;
    const auto* first=reinterpret_cast<const uint8_t*>(&shadow);
    // 이번 그림자 상수를 제출 전 사본으로 보관한다. 최종 캐시 확정은 공통 CommitFrameData가 맡는다.
    pendingShadowBytes.assign(first,first+sizeof(shadow));
    if(shadowReady && pendingShadowBytes==shadowBytes)return true; // 이미 올린 그림자 상수와 같으면 복사와 전환 기록을 생략한다.
    if(shadowReady) { const RHI::ResourceBarrierDesc barrier={shadowMatrixBuffer,nullptr,RHI::ResourceState::ConstantBuffer,RHI::ResourceState::CopyDestination,{}}; commandList.ResourceBarrier(&barrier,1); } // 재업로드가 필요한 기존 버퍼만 ConstantBuffer에서 CopyDestination으로 바꾼다.

	if(!device->UpdateBuffer( // 그림자 상수 복사 기록이 실패하면 상위 프레임이 실패를 처리하게 한다.
			commandList,
			shadowMatrixBuffer,
			0,
			&shadow,
			static_cast<uint32_t>(sizeof(shadow))))
		return false;
	// 복사 뒤 다음 그림자/조명 패스가 읽을 ConstantBuffer 상태를 기록한다.
	const RHI::ResourceBarrierDesc after = {
		shadowMatrixBuffer, nullptr,
		RHI::ResourceState::CopyDestination,
		RHI::ResourceState::ConstantBuffer,
		{}
	};
	commandList.ResourceBarrier(&after, 1);
	return true;
}
