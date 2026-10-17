#include "RendererInternal.h"
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

	Math::float4x4 ComputeStableCascadeViewProj(
		const Math::float3& lightDirection, Math::float3 center, float radius,
		const Math::Bounds3& sceneBounds, uint32_t resolution)
	{
		const auto light = Math::NormalizeOr(lightDirection, Math::float3(0, 0, -1));
		const auto right = Math::Normalize(Math::Cross(light * -1.f, SelectUpVector(light)));
		const auto up = Math::Normalize(Math::Cross(right, light * -1.f));
		// Radius is independent of camera rotation. Reserve one texel for snapping.
		const float diameter = 2 * radius * (resolution > 1 ? float(resolution) / (resolution - 1) : 1);
		const float texel = diameter / std::max(1u, resolution);
		const float x = std::round(Math::Dot(center, right) / texel) * texel;
		const float y = std::round(Math::Dot(center, up) / texel) * texel;
		float minZ = Math::Dot(center, light) - radius;
		float maxZ = Math::Dot(center, light) + radius;
		if(sceneBounds.valid)
		{
			minZ = std::numeric_limits<float>::max();
			maxZ = -minZ;
			for(uint32_t corner = 0; corner < 8; ++corner)
			{
				const Math::float3 point{
					corner & 1 ? sceneBounds.max.x : sceneBounds.min.x,
					corner & 2 ? sceneBounds.max.y : sceneBounds.min.y,
					corner & 4 ? sceneBounds.max.z : sceneBounds.min.z};
				const float z = Math::Dot(point, light);
				minZ = std::min(minZ, z);
				maxZ = std::max(maxZ, z);
			}
		}
		// Keep light-space depth tied to scene casters/receivers, not the camera.
		// Off-screen roofs and walls can still cast into the receiver's XY region.
		constexpr float padding = .5f, nearPlane = .1f;
		// Construct from fixed axes directly: subtracting origin-light in LookAt
		// would introduce position-dependent cancellation into the light rotation.
		Math::float4x4 view = {};
		view.m[0]=right.x;view.m[4]=right.y;view.m[8]=right.z;view.m[12]=-x;
		view.m[1]=up.x;view.m[5]=up.y;view.m[9]=up.z;view.m[13]=-y;
		view.m[2]=light.x;view.m[6]=light.y;view.m[10]=light.z;
		view.m[14]=-(maxZ+padding+nearPlane);view.m[15]=1;
		auto projection = Math::OrthographicRH_ZO(diameter, diameter, nearPlane,
			std::max(maxZ - minZ + 2 * padding + nearPlane, nearPlane + .1f));
		projection.m[5] = -projection.m[5];
		return projection * view;
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
		const Math::float3 normalizedDirection = Math::NormalizeOr(lightDirection, {0,0,-1});
		if(bounds.valid && !(boundsMin.x > boundsMax.x || boundsMin.y > boundsMax.y || boundsMin.z > boundsMax.z))
		{
			const Math::float3 center = bounds.Center();
			const Math::float3 halfExtent = bounds.Extent();
			const float radius = std::max(Length(halfExtent), 0.1f);

			sceneCenter = center;
			lightDistance = std::max(radius + 0.25f + 0.1f, 0.5f);

			const Math::float3 lightOrigin = sceneCenter + normalizedDirection * lightDistance;
			const Math::float4x4 view = Math::LookAtRH(lightOrigin, sceneCenter, SelectUpVector(normalizedDirection));

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

			const float limit = std::numeric_limits<float>::max();
			Math::Bounds3 lightBounds{{limit, limit, limit}, {-limit, -limit, -limit}, true};
			for(const Math::float3& corner : corners)
			{
				lightBounds.Include(Math::TransformPoint(view, corner));
			}

			orthoWidth = std::max(lightBounds.max.x - lightBounds.min.x + 0.25f * 2.0f, 0.1f);
			orthoHeight = std::max(lightBounds.max.y - lightBounds.min.y + 0.25f * 2.0f, 0.1f);
			const float depthRange = std::max(lightBounds.max.z - lightBounds.min.z + 0.25f * 2.0f, 0.1f);
			farPlane = std::max(0.1f + depthRange + lightDistance, 0.1f + 0.1f);
		}

		// 캐스케이드 타일의 텍셀에 중심을 맞춘 뒤 최종 광원 행렬을 만든다.
		if(tileResolution > 1u)
		{
			// Reserve half a texel on each side for the largest center snap.
			const float snapPadding = float(tileResolution) / float(tileResolution - 1u);
			orthoWidth *= snapPadding;
			orthoHeight *= snapPadding;
			const Math::float3 lightForward = normalizedDirection;
			const Math::float3 viewForward = lightForward * -1.0f;
			const Math::float3 right = Math::NormalizeOr(Math::Cross(viewForward, SelectUpVector(lightForward)), Math::float3(1.0f, 0.0f, 0.0f));
			const Math::float3 up = Math::NormalizeOr(Math::Cross(right, viewForward), Math::float3(0.0f, 1.0f, 0.0f));
			const float texelX = orthoWidth / static_cast<float>(tileResolution);
			const float texelY = orthoHeight / static_cast<float>(tileResolution);
			const float centerX = Math::Dot(sceneCenter, right);
			const float centerY = Math::Dot(sceneCenter, up);
			const float snappedX = std::round(centerX / texelX) * texelX;
			const float snappedY = std::round(centerY / texelY) * texelY;
			sceneCenter = sceneCenter + right * (snappedX - centerX) + up * (snappedY - centerY);
		}

		// lightDirection은 표면에서 광원으로 향하므로 광원 카메라를 그 방향에 둔다.
		const Math::float3 lightOrigin = sceneCenter + normalizedDirection * lightDistance;

		const Math::float3 up = SelectUpVector(normalizedDirection);
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
		// 작은 광원 범위에서도 near*far 곱의 언더플로 없이 같은 투영을 계산한다.
		projection.m[10] = 1.f / (nearPlane / farPlane - 1.f);
		projection.m[14] = nearPlane / (nearPlane / farPlane - 1.f);
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
		float nearPlane,
		float farPlane)
	{
		const Math::float3 lightForward = Normalize(lightDirection);
		const Math::float3 target = lightPosition + lightForward;
		const Math::float4x4 view = Math::LookAtRH(lightPosition, target, SelectUpVector(lightForward));
		// 그림자 깊이 패스/샘플링은 Y-down 광원 투영 기준으로 튜닝돼 있다(카메라 캐노니컬과 무관).
		Math::float4x4 proj = Math::PerspectiveRH_ZO(fovYRadians, 1.0f, nearPlane, farPlane);
		proj.m[10] = 1.f / (nearPlane / farPlane - 1.f);
		proj.m[14] = nearPlane / (nearPlane / farPlane - 1.f);
		// The shared math helper caps FOV at 3 radians. A spot cone can reach
		// 89 degrees per side, so retain its actual focal coefficient here.
		proj.m[0] = 1.f/std::tan(fovYRadians*.5f);
		proj.m[5] = -proj.m[0];
		return proj * view;
	}

	bool IsFinitePosition(const Math::float3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	Math::float4 ShadowProjectionParams(const Math::float4x4& matrix,float nearPlane=0,float farPlane=0)
	{
		const float x=.5f*Math::Length(Math::float3(matrix.m[0],matrix.m[4],matrix.m[8]));
		const float y=.5f*Math::Length(Math::float3(matrix.m[1],matrix.m[5],matrix.m[9]));
		if(nearPlane>0 && farPlane>nearPlane)
		{
			const float denominator=nearPlane/farPlane-1.f;
			return {x,y,1.f/denominator,nearPlane/denominator};
		}
		const float depthScale=Math::Length(Math::float3(matrix.m[2],matrix.m[6],matrix.m[10]));
		return {x,y,depthScale>0?1.f/depthScale:0.f,0};
	}

	bool LocalShadowClipRange(float range, float& nearPlane)
	{
		if(!std::isfinite(range) || range <= 0) return false;
		// 광원 범위보다 큰 고정 near 평면이 가까운 차폐물을 지우지 않도록 한다.
		nearPlane = std::min(.1f, range * .001f);
		return nearPlane > 0 && nearPlane < range;
	}

	void IncludeTransformedBounds(Math::Bounds3& bounds, const Math::Bounds3& local,
		const Math::float4x4& transform)
	{
		if(!local.valid) return;
		for(uint32_t corner = 0; corner < 8; ++corner)
			bounds.Include(Math::TransformPoint(transform, {
				corner & 1 ? local.max.x : local.min.x,
				corner & 2 ? local.max.y : local.min.y,
				corner & 4 ? local.max.z : local.min.z}));
	}

}

bool Renderer::Impl::ReuseShadowAtlas(const Scene& scene,const ShadowData& shadows,const std::vector<RendererDrawDesc>* draws)
{
    auto& cache=shadowAtlasCache;
    cache.pending=false;
    cache.reused=false;
    // 방향광도 실제 광원 행렬과 차폐물이 같으면 재사용한다. 시점 행렬은 샘플링에만 쓰인다.
    if(!config.lighting.reuseStaticShadows || (draws && !stockDrawInputs) || !UsesStockGeometry() ||
        !shadows.viewCount || !shadowPipeline || !shadowDepthTarget || !shadowMatrixBuffer)
    {
        cache.valid=false;
        return false;
    }

    auto& bytes=cache.pendingBytes;
    bytes.clear();cache.pendingMeshes.clear();
    // Append initialized scalars/arrays only: padding in public descriptors is
    // not part of the depth result and must not cause spurious cache misses.
    const auto append=[&](const auto& value) {
        const auto* first=reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(),first,first+sizeof(value));
    };
    const auto appendPosition=[&](const Math::float3& value) {
        append(value.x);append(value.y);append(value.z);
    };
    append(config.lighting.shadowQuality);append(config.enableFrustumCulling);append(config.enableInstancing);
    append(config.lighting.shadowDistance);append(config.lighting.cascadeSplitLambda);
    append(config.lighting.maxDirectionalShadowLights);append(config.lighting.maxPointShadowLights);
    append(config.lighting.maxSpotShadowLights);
    append(shadows.viewCount);append(shadows.columns);append(shadows.rows);append(shadows.resolution);
    for(uint32_t i=0;i<shadows.viewCount;++i)
    {
        append(shadows.constants.lightViewProjectionMatrix[i].m);
        const auto& rect=shadows.constants.atlasRect[i];
        append(rect.x);append(rect.y);append(rect.z);append(rect.w);
    }
    // 비활성 광원까지 선택 상태를 기록한다. 색과 양수 강도의 크기는 깊이를 바꾸지 않는다.
    const auto appendLight=[&](const auto& light) {
        append(light.enabled);append(light.priority);append(light.castShadow);
        const bool shadowVisible=light.shadowStrength>0;append(shadowVisible);
        const bool emitsLight=light.intensity>0;append(emitsLight);
        appendPosition(light.position);append(light.range);
    };
    append(scene.PointLights().size());
    for(const auto& light:scene.PointLights())appendLight(light);
    append(scene.SpotLights().size());
    for(const auto& light:scene.SpotLights())
    {
        appendLight(light);appendPosition(light.direction);
        append(light.innerConeRadians);append(light.outerConeRadians);
    }

    append(scene.GetEntityCount());
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const bool casts=scene.GetEntityLighting(entity).castShadow;
        append(casts);
        if(!casts)continue;
        const auto meshId=scene.GetEntityMesh(entity);
        const auto materialId=scene.GetEntityMaterial(entity);
        append(meshId);append(materialId);
        if(!IsValid(meshId) || !IsValid(materialId) || ToIndex(meshId)>=m_meshes.size() ||
            ToIndex(materialId)>=materialStates.size())continue;
        const auto& mesh=m_meshes[ToIndex(meshId)];
        append(mesh.prepared);
        if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount)continue;
        const auto& material=scene.Materials()[ToIndex(materialId)];
        append(material.alphaMode);
        if(material.alphaMode==MaterialAlphaMode::Blend)continue;
        cache.pendingMeshes.push_back(mesh.source);
        append(reinterpret_cast<uintptr_t>(mesh.vertexBuffer));
        if(draws)
        {
            const auto& draw=(*draws)[i];
            // 변경 번호가 없는 외부 정점 버퍼의 내용은 포인터만으로 재사용을 보장할 수 없다.
            if(draw.vertexBuffer && !draw.geometryRevision)
            {
                cache.valid=false;
                return false;
            }
            append(reinterpret_cast<uintptr_t>(draw.vertexBuffer));
            append(draw.geometryRevision);
            // 같은 형상이라도 경계 변화가 캐스터 컬링 결과를 바꿀 수 있다.
            append(draw.hasLocalBounds);
            if(draw.hasLocalBounds)
            {
                append(draw.localBounds.valid);
                if(draw.localBounds.valid)
                {
                    appendPosition(draw.localBounds.min);
                    appendPosition(draw.localBounds.max);
                }
            }
        }
        append(reinterpret_cast<uintptr_t>(mesh.indexBuffer));append(mesh.indexCount);
        append(scene.GetTransform(entity).m);
        append(material.baseColor.w);append(material.alphaCutoff);append(material.doubleSided);
        append(reinterpret_cast<uintptr_t>(materialStates[ToIndex(materialId)].textures[0]));
        // TextureCache retains immutable source pixels across frames. Include
        // their identity as well as the native handle, whose address may recur.
        append(reinterpret_cast<uintptr_t>(material.baseColorTexture.GetPixels().data()));
        append(material.baseColorTexture.GetWidth());append(material.baseColorTexture.GetHeight());
        append(material.baseColorTexture.GetColorSpace());
    }
    cache.pending=true;
    if(!cache.valid || cache.target!=shadowDepthTarget || cache.scene.expired() ||
        cache.scene.owner_before(scene.m_lifetime) || scene.m_lifetime.owner_before(cache.scene) ||
        cache.bytes!=bytes || cache.meshes.size()!=cache.pendingMeshes.size())return false;
    for(size_t i=0;i<cache.meshes.size();++i)
        if(cache.meshes[i].expired() || cache.meshes[i].owner_before(cache.pendingMeshes[i]) ||
            cache.pendingMeshes[i].owner_before(cache.meshes[i]))return false;
    cache.reused=true;
    return true;
}

void Renderer::Impl::CommitShadowAtlas(const Scene& scene)
{
    auto& cache=shadowAtlasCache;
    if(!cache.pending)return;
    cache.bytes.swap(cache.pendingBytes);cache.meshes.swap(cache.pendingMeshes);
    cache.scene=scene.m_lifetime;cache.target=shadowDepthTarget;
    cache.valid=true;cache.pending=false;
}

// 그림자는 광원과 카메라로 행렬을 계산한 뒤 RHI 깊이 패스를 구성한다.
void Renderer::Impl::BuildShadows(ShadowData& state,const Scene& scene,const Camera& camera,
    const std::vector<RendererDrawDesc>* draws)
{
    const auto quality=config.lighting.shadowQuality;
    const uint32_t resolution=quality==ShadowQuality::Low?1024:quality==ShadowQuality::High?4096:2048;
    const uint32_t atlasResolution=quality==ShadowQuality::Low?2048:quality==ShadowQuality::High?8192:4096;
    const uint32_t cascadeCount=quality==ShadowQuality::Low?1:4;
    auto& constants=state.constants;auto& viewCount=state.viewCount;auto& columns=state.columns;auto& rows=state.rows;
    constants={};viewCount=0;columns=rows=state.resolution=1;state.directionalDistance=0;
    constants.cameraView=camera.view;
    // 뷰별 투영 계수로 깊이를 실제 거리로 복원한 뒤 반그림자 폭으로 변환한다.
    constants.filterParams={static_cast<float>(config.lighting.shadowBlockerSamples),
        static_cast<float>(config.lighting.shadowFilterSamples),quality==ShadowQuality::High?2.f:1.f,0};
    constants.softShadowParams={config.lighting.shadowMaxFilterRadius,0,0,0};
    if(!config.lighting.enabled||!config.lighting.direct||!config.lighting.shadows)return;
    const auto directional=SelectActiveLightIndices(scene.DirectionalLights(),static_cast<uint32_t>(std::size(constants.directionalViews)));
    const auto points=SelectActiveLightIndices(scene.PointLights(),static_cast<uint32_t>(std::size(constants.pointViews)));
    const auto spots=SelectActiveLightIndices(scene.SpotLights(),static_cast<uint32_t>(std::size(constants.spotViews)));
    const auto& projection=camera.projection;
    const bool finiteProjection=std::all_of(std::begin(projection.m),std::end(projection.m),
        [](float value){return std::isfinite(value);});
    const bool centeredProjection=finiteProjection &&
        projection.m[0]>0 && projection.m[5]>0 &&
        projection.m[1]==0 && projection.m[2]==0 && projection.m[3]==0 &&
        projection.m[4]==0 && projection.m[6]==0 && projection.m[7]==0 &&
        projection.m[8]==0 && projection.m[9]==0 && projection.m[12]==0 && projection.m[13]==0 &&
        std::isfinite(1/projection.m[0]) && std::isfinite(1/projection.m[5]);
    const bool orthographic=centeredProjection && projection.m[11]==0 && projection.m[15]==1;
    const bool perspective=centeredProjection && projection.m[11]==-1 && projection.m[15]==0;
    const float cameraNear=projection.m[14]/projection.m[10];
    const float cameraFar=orthographic?(projection.m[14]-1)/projection.m[10]:projection.m[14]/(projection.m[10]+1);
    const bool validCameraRange=std::isfinite(cameraNear) && std::isfinite(cameraFar) && cameraFar>cameraNear &&
        (orthographic || (perspective && cameraNear>0));
    const bool explicitDistance=config.lighting.shadowDistance>0;
    const float shadowFar=explicitDistance?std::min(cameraFar,config.lighting.shadowDistance):cameraFar;
    // 기본 저품질·직교 투영의 장면 맞춤은 유지하고 명시적 거리에는 카메라 구간을 맞춘다.
    const bool useCameraFit=validCameraRange && ((perspective && cascadeCount>1) || explicitDistance);
    const bool directionalRangeValid=!useCameraFit || shadowFar>cameraNear;
    if(useCameraFit && directionalRangeValid)
    {
        const float aspect=projection.m[5]/projection.m[0];
        if(std::fabs(cameraNear-shadowCascadeNear)>std::max(std::fabs(cameraNear),.0001f)*.0001f ||
            std::fabs(shadowFar-shadowCascadeFar)>std::max(std::fabs(shadowFar),.0001f)*.0001f ||
            std::fabs(aspect-shadowCascadeAspect)>.0001f ||
            shadowCascadeSplitLambda!=config.lighting.cascadeSplitLambda || shadowCascadeOrthographic!=orthographic ||
            shadowCascadeCount!=cascadeCount)
        {
            shadowCascadeRadii.fill(0);
            shadowCascadeNear=cameraNear;
            shadowCascadeFar=shadowFar;
            shadowCascadeAspect=aspect;
            shadowCascadeSplitLambda=config.lighting.cascadeSplitLambda;
            shadowCascadeOrthographic=orthographic;
            shadowCascadeCount=cascadeCount;
        }
        constants.filterParams.w=cameraNear;
    }
    // 직접광의 원본 슬롯은 그대로 두고 우선순위순으로 유효한 그림자만 예산에 넣는다.
    const auto selectShadowSlots=[](const auto& selected,const auto& lights,uint32_t budget,const auto& valid) {
        uint32_t slots=0,used=0;
        for(uint32_t slot=0;slot<selected.size() && used<budget;++slot)
        {
            const auto& light=lights[selected[slot]];
            if(!light.castShadow || !(light.shadowStrength>0) || !(light.intensity>0) || !valid(light))continue;
            slots|=1u<<slot;++used;
        }
        return slots;
    };
    const uint32_t directionalShadowSlots=directionalRangeValid?
        selectShadowSlots(directional,scene.DirectionalLights(),config.lighting.maxDirectionalShadowLights,
            [](const auto&){return true;}):0;
    const uint32_t pointShadowSlots=selectShadowSlots(points,scene.PointLights(),config.lighting.maxPointShadowLights,
        [](const auto& light){float nearPlane;return IsFinitePosition(light.position) && LocalShadowClipRange(light.range,nearPlane);});
    const uint32_t spotShadowSlots=selectShadowSlots(spots,scene.SpotLights(),config.lighting.maxSpotShadowLights,
        [](const auto& light){float nearPlane;return IsFinitePosition(light.position) && IsFinitePosition(light.direction) &&
            Math::Length(light.direction)>0 && LocalShadowClipRange(light.range,nearPlane);});
    uint32_t expectedViewCount=0;
    for(uint32_t slot=0;slot<directional.size();++slot)
        if(directionalShadowSlots&(1u<<slot))expectedViewCount+=useCameraFit?cascadeCount:1;
    for(uint32_t slot=0;slot<points.size();++slot)
        if(pointShadowSlots&(1u<<slot))expectedViewCount+=6;
    for(uint32_t slot=0;slot<spots.size();++slot)
        if(spotShadowSlots&(1u<<slot))++expectedViewCount;
    columns=std::max(1u,static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(expectedViewCount)))));
    rows=std::max(1u,(expectedViewCount+columns-1)/columns);
    // 직접광은 모두 유지하고 선택된 그림자의 품질별 전체 예산 안에서 타일 해상도를 조정한다.
    // 행렬 안정화와 깊이 텍스처·viewport가 이 해상도를 함께 사용한다.
    const auto atlasLimit=std::min<uint64_t>(atlasResolution,device->GetLimit(RHI::Limit::Texture2DDimension));
    state.resolution=static_cast<uint32_t>(std::min<uint64_t>(resolution,atlasLimit/std::max(columns,rows)));
    Math::Bounds3 bounds;
    // 정적 장면 맞춤은 원본 정점의 정확한 경계를 유지하고 변형 입력만 현재 경계를 사용한다.
    const bool needsSceneBounds=!useCameraFit && directionalShadowSlots!=0;
    if(needsSceneBounds)for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto mesh=scene.GetEntityMesh(entity);
        const auto flags=scene.GetEntityLighting(entity);
        if(!IsValid(mesh)||ToIndex(mesh)>=scene.Meshes().size()||(!flags.castShadow&&!flags.receiveShadow))continue;
        if(draws && (*draws)[i].hasLocalBounds && (*draws)[i].localBounds.valid)
        {
            IncludeTransformedBounds(bounds,(*draws)[i].localBounds,scene.GetTransform(entity));
            continue;
        }
        for(const auto& vertex:(*scene.Meshes()[ToIndex(mesh)]).vertices)
            bounds.Include(Math::TransformPoint(scene.GetTransform(entity),vertex.position));
    }
    // 카메라 구간 맞춤의 깊이는 화면 밖 차폐물과 현재 변형 경계까지 보수적으로 포함한다.
    if(useCameraFit && directionalShadowSlots)for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto mesh=scene.GetEntityMesh(entity);
        const auto flags=scene.GetEntityLighting(entity);
        if(!IsValid(mesh) || (!flags.castShadow && !flags.receiveShadow) || ToIndex(mesh)>=m_meshes.size())continue;
        const auto& local=draws && (*draws)[i].hasLocalBounds && (*draws)[i].localBounds.valid?
            (*draws)[i].localBounds:m_meshes[ToIndex(mesh)].bounds;
        IncludeTransformedBounds(bounds,local,scene.GetTransform(entity));
    }
    for(uint32_t i=0;i<directional.size();++i)
    {
        const auto& light=scene.DirectionalLights()[directional[i]];
        if(!(directionalShadowSlots&(1u<<i)))continue;
        uint32_t count=1;
        const uint32_t first=viewCount;
        if(useCameraFit)
        {
            const float nearPlane=cameraNear;
            const float farPlane=shadowFar;
            count=cascadeCount;
            auto& splits=constants.directionalSplits[i];
            for(uint32_t c=0;c<count;++c)
            {
                const float fraction=static_cast<float>(c+1u)/static_cast<float>(count);
                const float uniform=nearPlane+(farPlane-nearPlane)*fraction;
                // 직교 투영의 near가 0 이하이면 로그 분배 대신 선형 분배를 사용한다.
                const float logarithmic=nearPlane>0?static_cast<float>(std::exp(std::log(double(nearPlane))+
                    (std::log(double(farPlane))-std::log(double(nearPlane)))*fraction)):uniform;
                splits[c]=logarithmic*config.lighting.cascadeSplitLambda+uniform*(1-config.lighting.cascadeSplitLambda);
            }
            splits[count-1u]=farPlane;
            const Math::float3 forward=Math::Normalize(Math::float3(-camera.view.m[2],-camera.view.m[6],-camera.view.m[10]));
            for(uint32_t c=0;c<count;++c)
            {
                // 셰이더의 전환 혼합과 맞춰 직전 구간의 마지막 10%까지 덮는다.
                const float previousStart=c<2?nearPlane:splits[c-2];
                const float sliceNear=c==0?nearPlane:splits[c-1]-.1f*(splits[c-1]-previousStart);
                const float sliceFar=splits[c];
                const auto center=camera.position+forward*((sliceNear+sliceFar)*.5f);
                const float halfDepth=(sliceFar-sliceNear)*.5f;
                const float halfWidth=(orthographic?1.f:sliceFar)/projection.m[0];
                const float halfHeight=(orthographic?1.f:sliceFar)/projection.m[5];
                const float radius=std::sqrt(halfDepth*halfDepth+halfWidth*halfWidth+halfHeight*halfHeight);
                // 시야각 변화로 격자가 매 프레임 줄어들지 않도록 증가량만 양자화한다.
                shadowCascadeRadii[c]=std::max(shadowCascadeRadii[c],std::ceil(radius*16)/16);
                constants.lightViewProjectionMatrix[viewCount++]=
                    ComputeStableCascadeViewProj(light.direction,center,shadowCascadeRadii[c],bounds,state.resolution);
            }
            state.directionalDistance=farPlane;
        }
        else
        {
            constants.lightViewProjectionMatrix[viewCount++]=ComputeDirectionalLightViewProj(light.direction,bounds,0);
            // 기본 장면 맞춤에는 카메라 far 제한이 없으므로 실제 장면의 최대 시점 깊이를 표시한다.
            Math::Bounds3 viewBounds;
            IncludeTransformedBounds(viewBounds,bounds,camera.view);
            state.directionalDistance=viewBounds.valid?std::max(0.f,-viewBounds.min.z):
                (std::isfinite(cameraFar)?std::max(0.f,cameraFar):0.f);
        }
        const float radius=std::isfinite(light.angularRadius) && light.angularRadius>0 && light.angularRadius<1.57079632679f?
            std::tan(light.angularRadius):0.f;
        constants.directionalViews[i]={static_cast<float>(first),static_cast<float>(count),light.shadowStrength,radius};
        for(uint32_t view=first;view<viewCount;++view)
            constants.viewProjectionParams[view]=ShadowProjectionParams(constants.lightViewProjectionMatrix[view]);
    }
    for(uint32_t i=0;i<points.size();++i)
    {
        const auto& light=scene.PointLights()[points[i]];
        if(!(pointShadowSlots&(1u<<i)))continue;
        const float nearPlane=std::min(.1f,light.range*.001f);
        const auto matrices=ComputePointLightViewProjections(light.position,nearPlane,light.range);
        const float radius=std::isfinite(light.sourceRadius)?std::max(light.sourceRadius,0.f):0.f;
        constants.pointViews[i]={static_cast<float>(viewCount),6,light.shadowStrength,radius};
        for(const auto& matrix:matrices)
        {
            constants.lightViewProjectionMatrix[viewCount]=matrix;
            constants.viewProjectionParams[viewCount++]=ShadowProjectionParams(matrix,nearPlane,light.range);
        }
    }
    for(uint32_t i=0;i<spots.size();++i)
    {
        const auto& light=scene.SpotLights()[spots[i]];
        if(!(spotShadowSlots&(1u<<i)))continue;
        const float nearPlane=std::min(.1f,light.range*.001f);
        const float radius=std::isfinite(light.sourceRadius)?std::max(light.sourceRadius,0.f):0.f;
        constants.spotViews[i]={static_cast<float>(viewCount),1,light.shadowStrength,radius};
        const auto cone=EffectiveSpotCone(light.innerConeRadians,light.outerConeRadians);
        constants.lightViewProjectionMatrix[viewCount]=ComputeSpotLightViewProj(light.position,light.direction,
            std::max(cone.outer*2,.001f),nearPlane,light.range);
        constants.viewProjectionParams[viewCount]=ShadowProjectionParams(constants.lightViewProjectionMatrix[viewCount],nearPlane,light.range);
        ++viewCount;
    }
    for(uint32_t i=0;i<viewCount;++i)
        constants.atlasRect[i]={static_cast<float>(i%columns)/columns,static_cast<float>(i/columns)/rows,1.f/columns,1.f/rows};
}
}
