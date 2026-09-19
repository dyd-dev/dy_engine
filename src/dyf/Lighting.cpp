#include "ShaderLayout.h"
#include "dyf/Scene.h"
#include "dyf/Camera.h"
#include "dyf/RHI/IDevice.h"
#include "SceneAlgorithms.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
namespace dyf
{

namespace
{
	[[nodiscard]] Math::float3 SelectUpVector(const Math::float3& lightForward)
	{
		// LookAt에서 forward와 up이 평행하면 right 벡터가 0 → 행렬 깨짐.
		// Z-up이 기본, 빛이 거의 수직이면 Y-up으로 폴백.
		if(std::abs(lightForward.z) > 0.95f)
		{
			return Math::float3(0.0f, 1.0f, 0.0f);
		}
		return Math::float3(0.0f, 0.0f, 1.0f);
	}

	void IncludeFrustumSlice(
		const Camera& camera,
		float sliceNear,
		float sliceFar,
		Math::Bounds3& bounds)
	{
		const Math::float3 forward = Math::NormalizeOr(Math::float3(-camera.view.m[2], -camera.view.m[6], -camera.view.m[10]), Math::float3(0.0f, 1.0f, 0.0f));
		const Math::float3 right = Math::NormalizeOr(Math::Cross(forward, Math::float3(camera.view.m[1], camera.view.m[5], camera.view.m[9])), Math::float3(1.0f, 0.0f, 0.0f));
		const Math::float3 up = Math::NormalizeOr(Math::Cross(right, forward), Math::float3(0.0f, 0.0f, 1.0f));
		const float fovYRadians = 2 * std::atan(1 / camera.projection.m[5]);
		const float tangent = std::tan(std::clamp(fovYRadians * 0.5f, 0.05f, 1.5f));
		const float safeAspect = std::max(camera.projection.m[5] / camera.projection.m[0], 0.0001f);
		for(uint32_t planeIndex = 0u; planeIndex < 2u; ++planeIndex)
		{
			const float distance = planeIndex == 0u ? sliceNear : sliceFar;
			const float halfHeight = tangent * distance;
			const float halfWidth = halfHeight * safeAspect;
			const Math::float3 center = camera.position + forward * distance;
			bounds.Include(center + right * halfWidth + up * halfHeight);
			bounds.Include(center + right * halfWidth - up * halfHeight);
			bounds.Include(center - right * halfWidth + up * halfHeight);
			bounds.Include(center - right * halfWidth - up * halfHeight);
		}
	}

	Math::float4x4 ComputeDirectionalLightViewProj(
		const Math::float3& lightDirection,
		const Math::Bounds3& bounds,
		uint32_t tileResolution)
	{
		Math::float3 sceneCenter = Math::float3(0.0f, 0.0f, 0.0f);
		float lightDistance = 8.0f;
		float orthoWidth = 6.0f;
		float orthoHeight = 6.0f;
		float farPlane = 20.0f;
		const auto& boundsMin = bounds.min;
		const auto& boundsMax = bounds.max;
		if(bounds.valid && !(boundsMin.x > boundsMax.x || boundsMin.y > boundsMax.y || boundsMin.z > boundsMax.z))
		{
			const Math::float3 center(
				(boundsMin.x + boundsMax.x) * 0.5f,
				(boundsMin.y + boundsMax.y) * 0.5f,
				(boundsMin.z + boundsMax.z) * 0.5f);
			const Math::float3 halfExtent(
				(boundsMax.x - boundsMin.x) * 0.5f,
				(boundsMax.y - boundsMin.y) * 0.5f,
				(boundsMax.z - boundsMin.z) * 0.5f);
			const float radius = std::max(Length(halfExtent), 0.1f);

			const Math::float3 lightForward = Normalize(lightDirection);
			sceneCenter = center;
			lightDistance = std::max(radius + 0.25f + 0.1f, 0.5f);

			const Math::float3 lightOrigin(
				sceneCenter.x + lightForward.x * lightDistance,
				sceneCenter.y + lightForward.y * lightDistance,
				sceneCenter.z + lightForward.z * lightDistance);
			const Math::float4x4 view = Math::LookAtRH(lightOrigin, sceneCenter, SelectUpVector(lightForward));

			const Math::float3 corners[] = {
				Math::float3(boundsMin.x, boundsMin.y, boundsMin.z),
				Math::float3(boundsMax.x, boundsMin.y, boundsMin.z),
				Math::float3(boundsMin.x, boundsMax.y, boundsMin.z),
				Math::float3(boundsMax.x, boundsMax.y, boundsMin.z),
				Math::float3(boundsMin.x, boundsMin.y, boundsMax.z),
				Math::float3(boundsMax.x, boundsMin.y, boundsMax.z),
				Math::float3(boundsMin.x, boundsMax.y, boundsMax.z),
				Math::float3(boundsMax.x, boundsMax.y, boundsMax.z)
			};

			float minX = std::numeric_limits<float>::max();
			float minY = std::numeric_limits<float>::max();
			float minZ = std::numeric_limits<float>::max();
			float maxX = -std::numeric_limits<float>::max();
			float maxY = -std::numeric_limits<float>::max();
			float maxZ = -std::numeric_limits<float>::max();
			for(const Math::float3& corner : corners)
			{
				const Math::float3 lightSpace = Math::TransformPoint(view, corner);
				minX = std::min(minX, lightSpace.x);
				minY = std::min(minY, lightSpace.y);
				minZ = std::min(minZ, lightSpace.z);
				maxX = std::max(maxX, lightSpace.x);
				maxY = std::max(maxY, lightSpace.y);
				maxZ = std::max(maxZ, lightSpace.z);
			}

			orthoWidth = std::max(maxX - minX + 0.25f * 2.0f, 0.1f);
			orthoHeight = std::max(maxY - minY + 0.25f * 2.0f, 0.1f);
			const float depthRange = std::max(maxZ - minZ + 0.25f * 2.0f, 0.1f);
			farPlane = std::max(0.1f + depthRange + lightDistance, 0.1f + 0.1f);
		}

		// 캐스케이드 타일의 텍셀에 중심을 맞춘 뒤 최종 광원 행렬을 만든다.
		if(tileResolution != 0u)
		{
			const Math::float3 lightForward = Math::NormalizeOr(lightDirection, Math::float3(0.0f, 0.0f, 1.0f));
			const Math::float3 viewForward = lightForward * -1.0f;
			const Math::float3 right = Math::NormalizeOr(Math::Cross(viewForward, SelectUpVector(lightForward)), Math::float3(1.0f, 0.0f, 0.0f));
			const Math::float3 up = Math::NormalizeOr(Math::Cross(right, viewForward), Math::float3(0.0f, 1.0f, 0.0f));
			const float texelX = orthoWidth / static_cast<float>(tileResolution);
			const float texelY = orthoHeight / static_cast<float>(tileResolution);
			if(!(texelX <= 0.0f || texelY <= 0.0f))
			{
				const float centerX = Math::Dot(sceneCenter, right);
				const float centerY = Math::Dot(sceneCenter, up);
				const float snappedX = std::round(centerX / texelX) * texelX;
				const float snappedY = std::round(centerY / texelY) * texelY;
				sceneCenter = sceneCenter + right * (snappedX - centerX) + up * (snappedY - centerY);
			}
		}

		const Math::float3 lightForward = Normalize(lightDirection);

		// lightDirection은 표면에서 광원으로 향하므로 광원 카메라를 그 방향에 둔다.
		const Math::float3 lightOrigin(
			sceneCenter.x + lightForward.x * lightDistance,
			sceneCenter.y + lightForward.y * lightDistance,
			sceneCenter.z + lightForward.z * lightDistance);

		const Math::float3 up = SelectUpVector(lightForward);
		const Math::float4x4 view = Math::LookAtRH(lightOrigin, sceneCenter, up);
		// 그림자 깊이 패스/샘플링은 Y-down 광원 투영 기준으로 튜닝돼 있다(카메라 캐노니컬과 무관).
		Math::float4x4 proj = Math::OrthographicRH_ZO(orthoWidth, orthoHeight, 0.1f, farPlane);
		proj.m[5] = -proj.m[5];

		return proj * view;
	}

	std::array<Math::float4x4, 6> ComputePointLightViewProjections(
		const Math::float3& lightPosition,
		float nearPlane,
		float farPlane)
	{
		nearPlane = std::max(nearPlane, 0.0001f);
		farPlane = std::max(farPlane, nearPlane + 0.0001f);
		const std::array<Math::float3, 6> directions = {
			Math::float3(1.0f, 0.0f, 0.0f), Math::float3(-1.0f, 0.0f, 0.0f),
			Math::float3(0.0f, 1.0f, 0.0f), Math::float3(0.0f, -1.0f, 0.0f),
			Math::float3(0.0f, 0.0f, 1.0f), Math::float3(0.0f, 0.0f, -1.0f)
		};
		const std::array<Math::float3, 6> upVectors = {
			Math::float3(0.0f, 0.0f, -1.0f), Math::float3(0.0f, 0.0f, -1.0f),
			Math::float3(0.0f, 0.0f, 1.0f), Math::float3(0.0f, 0.0f, -1.0f),
			Math::float3(0.0f, -1.0f, 0.0f), Math::float3(0.0f, -1.0f, 0.0f)
		};
		Math::float4x4 projection = Math::PerspectiveRH_ZO(1.57079633f, 1.0f, nearPlane, farPlane);
		std::array<Math::float4x4, 6> views = {};
		for(uint32_t face = 0u; face < 6; ++face)
		{
			views[face] = projection * Math::LookAtRH(lightPosition, lightPosition + directions[face], upVectors[face]);
		}
		return views;
	}

	Math::float4x4 ComputeSpotLightViewProj(
		const Math::float3& lightPosition,
		const Math::float3& lightDirection,
		float fovYRadians,
		float farPlane)
	{
		const Math::float3 lightForward = Normalize(lightDirection);
		const Math::float3 target(
			lightPosition.x + lightForward.x,
			lightPosition.y + lightForward.y,
			lightPosition.z + lightForward.z);
		const Math::float4x4 view = Math::LookAtRH(lightPosition, target, SelectUpVector(lightForward));
		// 그림자 깊이 패스/샘플링은 Y-down 광원 투영 기준으로 튜닝돼 있다(카메라 캐노니컬과 무관).
		Math::float4x4 proj = Math::PerspectiveRH_ZO(fovYRadians, 1.0f, 0.1f, farPlane);
		proj.m[5] = -proj.m[5];
		return proj * view;
	}

}

// 그림자는 광원과 카메라로 행렬을 계산한 뒤 RHI 깊이 패스를 구성한다.
void Renderer::BuildShadows(ShadowData& state,const Scene& scene,const Camera& camera)
{
    const auto quality=config.lighting.shadowQuality;
    const uint32_t resolution=quality==ShadowQuality::Low?1024:quality==ShadowQuality::High?4096:2048;
    const uint32_t atlasResolution=quality==ShadowQuality::Low?2048:quality==ShadowQuality::High?8192:4096;
    const uint32_t cascadeCount=quality==ShadowQuality::Low?1:4;
    auto& constants=state.constants;auto& viewCount=state.viewCount;auto& columns=state.columns;auto& rows=state.rows;
    constants={};viewCount=0;columns=rows=state.resolution=1;
    constants.cameraView=camera.view;
    // 3x3 차폐물 탐색과 투영 깊이로 소프트 섀도 반경을 추정한다.
    constants.filterParams={.05f,2,quality==ShadowQuality::Low?4.f:8.f,0};
    if(!config.lighting.enabled||!config.lighting.shadows)return;
    const auto directional=SelectActiveLightIndices(scene.DirectionalLights(),static_cast<uint32_t>(std::size(constants.directionalViews)));
    const auto points=SelectActiveLightIndices(scene.PointLights(),static_cast<uint32_t>(std::size(constants.pointViews)));
    const auto spots=SelectActiveLightIndices(scene.SpotLights(),static_cast<uint32_t>(std::size(constants.spotViews)));
    const bool useCascades=cascadeCount>1 && camera.projection.m[11]==-1.f;
    uint32_t expectedViewCount=0;
    for(const auto index:directional)
        if(scene.DirectionalLights()[index].castShadow)expectedViewCount+=useCascades?cascadeCount:1;
    for(const auto index:points)
        if(scene.PointLights()[index].castShadow)expectedViewCount+=6;
    for(const auto index:spots)
        if(scene.SpotLights()[index].castShadow)++expectedViewCount;
    columns=std::max(1u,static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(expectedViewCount)))));
    rows=std::max(1u,(expectedViewCount+columns-1)/columns);
    // 지원 광원은 모두 유지하고 품질별 전체 예산 안에서 타일 해상도만 조정한다.
    // 행렬 안정화와 깊이 텍스처·viewport가 이 해상도를 함께 사용한다.
    const auto atlasLimit=std::min<uint64_t>(atlasResolution,device->GetLimit(RHI::Limit::Texture2DDimension));
    state.resolution=static_cast<uint32_t>(std::min<uint64_t>(resolution,atlasLimit/std::max(columns,rows)));
    Math::Bounds3 bounds;
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto mesh=scene.GetEntityMesh(entity);
        const auto flags=scene.GetEntityLighting(entity);
        if(!IsValid(mesh)||(!flags.castShadow&&!flags.receiveShadow))continue;
        for(const auto& vertex:(*scene.Meshes()[ToIndex(mesh)]).vertices)
            bounds.Include(Math::TransformPoint(scene.GetTransform(entity).worldMatrix,vertex.position));
    }
    for(uint32_t i=0;i<directional.size();++i)
    {
        const auto& light=scene.DirectionalLights()[directional[i]];
        if(!light.castShadow)continue;
        uint32_t count=1;
        const uint32_t first=viewCount;
        if(useCascades)
        {
            const float cameraNear=camera.projection.m[14]/camera.projection.m[10];
            // 캐스케이드 범위는 카메라의 가시 범위를 따른다. 내부 초기값 20으로 자르지 않는다.
            const float cameraFar=camera.projection.m[14]/(camera.projection.m[10]+1);
            const float nearPlane=std::max(cameraNear,0.0001f);
            const float farPlane=std::max(cameraFar,nearPlane+0.0001f);
            count=std::clamp(cascadeCount,1u,4u);
            auto& splits=constants.directionalSplits[i];
            for(uint32_t c=0;c<count;++c)
            {
                const float fraction=static_cast<float>(c+1u)/static_cast<float>(count);
                const float logarithmic=nearPlane*std::pow(farPlane/nearPlane,fraction);
                const float uniform=nearPlane+(farPlane-nearPlane)*fraction;
                splits[c]=logarithmic*.65f+uniform*(1.0f-.65f);
            }
            for(uint32_t c=count;c<4;++c)splits[c]=farPlane;
            splits[count-1u]=farPlane;
            float sliceNear=std::max(cameraNear,0.0001f);
            for(uint32_t c=0;c<count;++c)
            {
                const float sliceFar=splits[c];
                Math::Bounds3 sliceBounds;
                IncludeFrustumSlice(camera,sliceNear,sliceFar,sliceBounds);
                constants.lightViewProjectionMatrix[viewCount++]=
                    ComputeDirectionalLightViewProj(light.direction,sliceBounds,state.resolution);
                sliceNear=sliceFar;
            }
        }
        else constants.lightViewProjectionMatrix[viewCount++]=ComputeDirectionalLightViewProj(light.direction,bounds,0);
        constants.directionalViews[i]={static_cast<float>(first),static_cast<float>(count),light.shadowStrength,0};
    }
    for(uint32_t i=0;i<points.size();++i)
    {
        const auto& light=scene.PointLights()[points[i]];
        if(!light.castShadow)continue;
        const auto matrices=ComputePointLightViewProjections(light.position,.1f,
            std::max(light.range,.1f+.001f));
        constants.pointViews[i]={static_cast<float>(viewCount),6,light.shadowStrength,0};
        for(const auto& matrix:matrices)constants.lightViewProjectionMatrix[viewCount++]=matrix;
    }
    for(uint32_t i=0;i<spots.size();++i)
    {
        const auto& light=scene.SpotLights()[spots[i]];
        if(!light.castShadow)continue;
        constants.spotViews[i]={static_cast<float>(viewCount),1,light.shadowStrength,0};
        constants.lightViewProjectionMatrix[viewCount++]=ComputeSpotLightViewProj(light.position,light.direction,
            std::clamp(light.outerConeRadians*2,.001f,3.13f),std::max(light.range,.1f+.001f));
    }
    for(uint32_t i=0;i<viewCount;++i)
        constants.atlasRect[i]={static_cast<float>(i%columns)/columns,static_cast<float>(i/columns)/rows,1.f/columns,1.f/rows};
}
}
