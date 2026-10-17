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
	[[nodiscard]] inline Math::float3 NormalizeDirection(const Math::float3& direction)
	{
		return Math::NormalizeOr(direction, Math::float3(0.0f, 0.0f, -1.0f));
	}

	[[nodiscard]] inline Math::float3 OrthogonalUp(const Math::float3& direction, const Math::float3& up)
	{
		const Math::float3 projected = up - direction * Math::Dot(up, direction);
		const Math::float3 fallback = std::fabs(direction.z) < 0.99f
			? Math::float3(0.0f, 0.0f, 1.0f)
			: Math::float3(0.0f, 1.0f, 0.0f);
		return Math::NormalizeOr(projected, Math::NormalizeOr(
			fallback - direction * Math::Dot(fallback, direction),
			Math::float3(1.0f, 0.0f, 0.0f)));
	}
}

bool Renderer::Impl::UpdateLightingBuffer(
	const Scene& scene,
	const Camera& camera,
	RHI::IDevice* device,
	RHI::ICommandList& commandList)
{
	if(lightingBuffer && lightingBuffer->GetDesc().size!=sizeof(RendererLightingConstants)) {lightingBuffer=nullptr;lightingReady=false;}
	if(!lightingBuffer) lightingBuffer = device->CreateBuffer(RHI::BufferDesc{
		static_cast<uint32_t>(sizeof(RendererLightingConstants)),
		static_cast<uint32_t>(sizeof(RendererLightingConstants)),
		RHI::BufferUsage::Constant,
		RHI::ResourceState::CopyDestination
	});
	if(lightingBuffer == nullptr) return false;

    // Retain capacity and reproduce stable priority ordering without stable_sort scratch.
    const auto selectIndices=[&](const auto& lights,uint32_t capacity,uint32_t slot)->const std::vector<uint32_t>& {
        auto& indices=lightingIndexScratch[slot];indices.clear();
        if(!config.lighting.direct) return indices;
        indices.reserve(lights.size());
        for(uint32_t i=0;i<lights.size();++i)if(lights[i].enabled)indices.push_back(i);
        std::sort(indices.begin(),indices.end(),[&](uint32_t a,uint32_t b) {
            return lights[a].priority==lights[b].priority ? a<b : lights[a].priority>lights[b].priority;
        });
        if(indices.size()>capacity)indices.resize(capacity);
        return indices;
    };
	RendererLightingConstants constants = {};
	const auto& directionalIndices=selectIndices(scene.DirectionalLights(),static_cast<uint32_t>(std::size(constants.directionalLights)),0);
	const auto& pointIndices=selectIndices(scene.PointLights(),static_cast<uint32_t>(std::size(constants.pointLights)),1);
	const DirectionalLight* light = directionalIndices.empty() ? nullptr : &scene.DirectionalLights()[directionalIndices.front()];
	const PointLight* pointLight = pointIndices.empty() ? nullptr : &scene.PointLights()[pointIndices.front()];
	const Math::float3 lightDirection = light != nullptr
		? (Math::LengthSquared(light->direction)<=1e-8f ? Math::float3(0,0,-1) : light->direction)
		: Math::float3(0.0f, 0.0f, 1.0f);
	const Math::float3 lightColor = light != nullptr
		? light->color
		: Math::float3(0.0f, 0.0f, 0.0f);
	const float lightIntensity = light != nullptr ? light->intensity : 0.0f;
	const bool shadowsEnabled = shadowPipeline != nullptr &&
		light != nullptr && light->castShadow;
	const float shadowStrength = shadowsEnabled ? light->shadowStrength : 0.0f;

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
	if(pointLight != nullptr)
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

		const uint32_t pointCount = static_cast<uint32_t>(pointIndices.size());
		for(uint32_t index = 0u; index < pointCount; ++index)
		{
			const PointLight& light = scene.PointLights()[pointIndices[index]];
			constants.pointLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.range, 0.0f)),
				Math::float4(light.color.x, light.color.y, light.color.z, std::max(light.intensity, 0.0f))
			};
		}

		const uint32_t spotCount = static_cast<uint32_t>(spotIndices.size());
		for(uint32_t index = 0u; index < spotCount; ++index)
		{
			const SpotLight& light = scene.SpotLights()[spotIndices[index]];
			// 기존 기본 셰이더 정책: 입력 각도를 정렬하고 89도까지 제한하여 cos 값으로 전달한다.
			const auto cone=EffectiveSpotCone(light.innerConeRadians,light.outerConeRadians);
			const Math::float3 direction = NormalizeDirection(light.direction);
			constants.spotLights[index] = {
				Math::float4(light.position.x, light.position.y, light.position.z, std::max(light.range, 0.0f)),
				Math::float4(direction.x, direction.y, direction.z, std::cos(cone.outer)),
				Math::float4(light.color.x, light.color.y, light.color.z, std::max(light.intensity, 0.0f)),
				Math::float4(std::cos(cone.inner), 0.0f, 0.0f, 0.0f)
			};
		}

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

		constants.lightCounts = Math::float4(static_cast<float>(directionalCount), static_cast<float>(pointCount), static_cast<float>(spotCount), config.lighting.enabled ? 1.0f : 0.0f);
		constants.areaLightCounts = Math::float4(static_cast<float>(rectAreaCount), static_cast<float>(discAreaCount), 0.0f, 0.0f);

        // 조명을 끄면 모든 광원 평가를 생략한다. 재질의 기본색과 발광은 셰이더에서 유지한다.
        if(!config.lighting.enabled) { constants.lightCounts = {}; constants.areaLightCounts = {}; }

        constants.shadowLight = {1,0,constants.cameraPosition.w,constants.directionalLightDirection.w};

    const auto* first=reinterpret_cast<const uint8_t*>(&constants);
    pendingLightingBytes.assign(first,first+sizeof(constants));
    if(lightingReady && pendingLightingBytes==lightingBytes)return true;
	if(lightingReady) { const RHI::ResourceBarrierDesc barrier={lightingBuffer,nullptr,RHI::ResourceState::ConstantBuffer,RHI::ResourceState::CopyDestination,{}}; commandList.ResourceBarrier(&barrier,1); }
	if(!device->UpdateBuffer(
			commandList,
			lightingBuffer,
			0,
			&constants,
			static_cast<uint32_t>(sizeof(constants))))
		return false;
	const RHI::ResourceBarrierDesc after = {
		lightingBuffer, nullptr,
		RHI::ResourceState::CopyDestination,
		RHI::ResourceState::ConstantBuffer,
		{}
	};
	commandList.ResourceBarrier(&after, 1);
	return true;
}

bool Renderer::Impl::UpdateShadowBuffer(
	RHI::IDevice* device,
	RHI::ICommandList& commandList,const ShadowData& shadows)
{
	if(!config.lighting.enabled || !config.lighting.direct || !config.lighting.shadows) return true;
	if(shadowMatrixBuffer && shadowMatrixBuffer->GetDesc().size!=sizeof(RendererShadowConstants)) {shadowMatrixBuffer=nullptr;shadowReady=false;}
	if(!shadowMatrixBuffer) shadowMatrixBuffer = device->CreateBuffer(RHI::BufferDesc{
		static_cast<uint32_t>(sizeof(RendererShadowConstants)),
		static_cast<uint32_t>(sizeof(RendererShadowConstants)),
		RHI::BufferUsage::Constant,
		RHI::ResourceState::CopyDestination
	});
	if(shadowMatrixBuffer == nullptr) return false;

    const auto& shadow=shadows.constants;
    const auto* first=reinterpret_cast<const uint8_t*>(&shadow);
    pendingShadowBytes.assign(first,first+sizeof(shadow));
    if(shadowReady && pendingShadowBytes==shadowBytes)return true;
    if(shadowReady) { const RHI::ResourceBarrierDesc barrier={shadowMatrixBuffer,nullptr,RHI::ResourceState::ConstantBuffer,RHI::ResourceState::CopyDestination,{}}; commandList.ResourceBarrier(&barrier,1); }

	if(!device->UpdateBuffer(
			commandList,
			shadowMatrixBuffer,
			0,
			&shadow,
			static_cast<uint32_t>(sizeof(shadow))))
		return false;
	const RHI::ResourceBarrierDesc after = {
		shadowMatrixBuffer, nullptr,
		RHI::ResourceState::CopyDestination,
		RHI::ResourceState::ConstantBuffer,
		{}
	};
	commandList.ResourceBarrier(&after, 1);
	return true;
}
