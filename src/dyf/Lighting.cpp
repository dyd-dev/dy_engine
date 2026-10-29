#include "RendererInternal.h"
#include "ImageDds.h"
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
	// LookAt의 전방과 up이 평행해 오른쪽 축이 0이 되지 않도록 기본 Z-up을 쓰고, |lightForward.z|>0.95이면 Y-up으로 바꾼다.
	[[nodiscard]] Math::float3 SelectUpVector(const Math::float3& lightForward)
	{
		if(std::abs(lightForward.z) > 0.95f)
		{
			return Math::float3(0.0f, 1.0f, 0.0f);
		}
		return Math::float3(0.0f, 0.0f, 1.0f);
	}

	// 카메라의 한 거리 구간을 감싸는 구로 방향광 캐스케이드를 만든다. 회전과 무관한 반경·고정 광원 축·텍셀 스냅으로 그림자 흔들림을 줄인다.
	Math::float4x4 ComputeStableCascadeViewProj(
		const Math::float3& lightDirection, Math::float3 center, float radius,
		const Math::Bounds3& sceneBounds, uint32_t resolution)
	{
		const auto light = Math::NormalizeOr(lightDirection, Math::float3(0, 0, -1));
		// 표면에서 광원으로 향하는 light의 반대 방향을 카메라 전방으로 사용한다.
		// 두 벡터 모두에 수직인 벡터를 구하는 연산(외적)으로 전방과 기준 up에서 right를 구하고, right와 전방에서 up을 구한 뒤 각각 정규화한다.
		const auto right = Math::Normalize(Math::Cross(light * -1.f, SelectUpVector(light)));
		const auto up = Math::Normalize(Math::Cross(right, light * -1.f));
		// 카메라 회전과 무관한 반경에 전체 폭 한 텍셀의 여유를 더해 중심이 최대 반 텍셀 스냅되어도 경계가 잘리지 않게 한다.
		const float diameter = 2 * radius * (resolution > 1 ? float(resolution) / (resolution - 1) : 1);
		const float texel = diameter / std::max(1u, resolution);
		// 두 벡터의 대응 성분을 곱해 더하는 연산(내적)으로 중심의 right/up 방향 좌표를 구한 뒤 텍셀 간격으로 반올림한다.
		// 미세한 카메라 이동이 즉시 그림자 격자를 움직이지 않게 하며, 아래의 light 방향 내적은 깊이 범위를 정한다.
		const float x = std::round(Math::Dot(center, right) / texel) * texel;
		const float y = std::round(Math::Dot(center, up) / texel) * texel;
		float minZ = Math::Dot(center, light) - radius;
		float maxZ = Math::Dot(center, light) + radius;
		if(sceneBounds.valid) // 장면 경계가 있으면 구 자체의 깊이 대신 모든 관련 차폐물·수광체를 담는 광원 깊이 범위를 사용한다.
		{
			minZ = std::numeric_limits<float>::max();
			maxZ = -minZ;
			for(uint32_t corner = 0; corner < 8; ++corner)
			{
				const Math::float3 point{
					corner & 1 ? sceneBounds.max.x : sceneBounds.min.x,
					corner & 2 ? sceneBounds.max.y : sceneBounds.min.y,
					corner & 4 ? sceneBounds.max.z : sceneBounds.min.z};
				// 각 좌표축에 나란한 면으로 물체를 감싸는 상자(AABB)의 여덟 꼭짓점을 광원 축에 투영해 장면의 minZ/maxZ를 구한다. 화면 밖 물체도 해당 XY 영역에 그림자를 만들 수 있다.
				const float z = Math::Dot(point, light);
				minZ = std::min(minZ, z);
				maxZ = std::max(maxZ, z);
			}
		}
		constexpr float padding = .5f, nearPlane = .1f;
		// 이미 정한 축을 행렬에 직접 넣는다. 큰 월드 좌표에서 가까운 두 점을 빼 LookAt 방향을 구할 때 생기는 상쇄 오차를 피한다.
		Math::float4x4 view = {};
		view.m[0]=right.x;view.m[4]=right.y;view.m[8]=right.z;view.m[12]=-x;
		view.m[1]=up.x;view.m[5]=up.y;view.m[9]=up.z;view.m[13]=-y;
		view.m[2]=light.x;view.m[6]=light.y;view.m[10]=light.z;
		view.m[14]=-(maxZ+padding+nearPlane);view.m[15]=1;
		// 광원 깊이 범위 앞뒤에 여백을 더한 직교 투영이다. 이후 Y 부호 반전은 그림자 깊이/샘플링 규약에 맞춘다.
		auto projection = Math::OrthographicRH_ZO(diameter, diameter, nearPlane,
			std::max(maxZ - minZ + 2 * padding + nearPlane, nearPlane + .1f));
		projection.m[5] = -projection.m[5];
		return projection * view;
	}

	// 캐스케이드 카메라 구간을 쓰지 않는 경로에서 장면 경계 전체에 맞춘 방향광 직교 투영을 만든다.
	Math::float4x4 ComputeDirectionalLightViewProj(
		const Math::float3& lightDirection,
		const Math::Bounds3& bounds,
		uint32_t tileResolution)
	{
		Math::float3 sceneCenter = Math::float3(0.0f, 0.0f, 0.0f);
		// 유효 경계가 없을 때 사용할 기본 위치·투영 크기다. 경계가 있으면 아래에서 실제 장면에 맞게 모두 다시 계산한다.
		float lightDistance = 8.0f;
		float orthoWidth = 6.0f;
		float orthoHeight = 6.0f;
		float farPlane = 20.0f;
		const auto& boundsMin = bounds.min;
		const auto& boundsMax = bounds.max;
		const Math::float3 normalizedDirection = Math::NormalizeOr(lightDirection, {0,0,-1});
		if(bounds.valid && !(boundsMin.x > boundsMax.x || boundsMin.y > boundsMax.y || boundsMin.z > boundsMax.z)) // 경계가 유효하고 min<=max이면 장면 중심과 여덟 꼭짓점으로 투영 범위를 계산한다.
		{
			const Math::float3 center = bounds.Center();
			const Math::float3 halfExtent = bounds.Extent();
			// 상자의 반폭·반높이·반깊이 벡터 길이는 중심에서 꼭짓점까지의 거리이므로 장면을 감싸는 구의 반경으로 쓴다. 반경을 최소 0.1로 두어 크기가 0인 장면도 투영 범위를 갖게 한다.
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
			// 월드 경계의 8개 꼭짓점을 광원 시점으로 옮겨 그 좌표계에서 필요한 폭·높이·깊이를 구한다.
			Math::Bounds3 lightBounds{{limit, limit, limit}, {-limit, -limit, -limit}, true};
			for(const Math::float3& corner : corners)
			{
				lightBounds.Include(Math::TransformPoint(view, corner));
			}

			// 광원 공간의 실제 범위에 양쪽 여백을 더한다. 월드 축의 폭을 그대로 쓰면 회전한 광원에서 장면이 잘릴 수 있다.
			orthoWidth = std::max(lightBounds.max.x - lightBounds.min.x + 0.25f * 2.0f, 0.1f);
			orthoHeight = std::max(lightBounds.max.y - lightBounds.min.y + 0.25f * 2.0f, 0.1f);
			const float depthRange = std::max(lightBounds.max.z - lightBounds.min.z + 0.25f * 2.0f, 0.1f);
			farPlane = std::max(0.1f + depthRange + lightDistance, 0.1f + 0.1f);
		}

		// tileResolution이 2 이상이면 광원 중심을 텍셀 간격에 맞춘다. 이 장면 맞춤 함수의 현재 호출부는 0을 전달하므로 아래 스냅을 적용하지 않는다.
		if(tileResolution > 1u)
		{
			// 한 번의 중심 반올림으로 생길 수 있는 양끝 반 텍셀 이동을 허용하도록 폭과 높이를 먼저 확장한다.
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
			// 광원 평면의 스냅 차이만 월드 중심에 반영한다. 광원 깊이 방향은 이 보정으로 바꾸지 않는다.
			sceneCenter = sceneCenter + right * (snappedX - centerX) + up * (snappedY - centerY);
		}

		// lightDirection은 표면에서 광원으로 향하므로 광원 카메라를 그 방향에 둔다.
		const Math::float3 lightOrigin = sceneCenter + normalizedDirection * lightDistance;

		const Math::float3 up = SelectUpVector(normalizedDirection);
		const Math::float4x4 view = Math::LookAtRH(lightOrigin, sceneCenter, up);
		// 그림자 깊이 기록과 조회에 사용하는 Y 방향을 맞추기 위해 직교 투영의 Y 배율 부호를 뒤집는다.
		Math::float4x4 proj = Math::OrthographicRH_ZO(orthoWidth, orthoHeight, 0.1f, farPlane);
		proj.m[5] = -proj.m[5];

		return proj * view;
	}

	// 점광원은 모든 방향으로 비추므로 ±X/±Y/±Z의 90도 원근 시점 여섯 개가 필요하다. 아틀라스에서도 여섯 타일을 차지한다.
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
		// 각 면의 회전과 이웃 면 방향을 맞추기 위한 고정 up축이다. 환경 색상 캡처의 면 up 규칙과 혼동하지 않는다.
		const std::array<Math::float3, 6> upVectors = {
			Math::float3(0.0f, 0.0f, -1.0f), Math::float3(0.0f, 0.0f, -1.0f),
			Math::float3(0.0f, 0.0f, 1.0f), Math::float3(0.0f, 0.0f, -1.0f),
			Math::float3(0.0f, -1.0f, 0.0f), Math::float3(0.0f, -1.0f, 0.0f)
		};
		Math::float4x4 projection = Math::PerspectiveRH_ZO(1.57079633f, 1.0f, nearPlane, farPlane);
		// near*far 대신 near/far 비율로 깊이 계수를 계산해 작은 광원 범위에서 발생하는 곱셈 언더플로를 줄인다.
		projection.m[10] = 1.f / (nearPlane / farPlane - 1.f);
		projection.m[14] = nearPlane / (nearPlane / farPlane - 1.f);
		std::array<Math::float4x4, 6> views = {};
		for(uint32_t face = 0u; face < 6; ++face)
		{
			views[face] = projection * Math::LookAtRH(lightPosition, lightPosition + directions[face], upVectors[face]);
		}
		return views;
	}

	// 스폿은 위치에서 광원 진행 방향을 바라보는 한 원근 시점을 사용한다. 전체 시야각은 바깥 원뿔 반각의 두 배다.
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
		// 스폿 투영의 깊이 계수도 near*far 대신 near/far로 계산해 작은 광원 범위의 언더플로를 줄인다. Y 배율은 아래에서 그림자 조회 방향에 맞춰 음수로 넣는다.
		Math::float4x4 proj = Math::PerspectiveRH_ZO(fovYRadians, 1.0f, nearPlane, farPlane);
		proj.m[10] = 1.f / (nearPlane / farPlane - 1.f);
		proj.m[14] = nearPlane / (nearPlane / farPlane - 1.f);
		// 공통 원근 함수의 3라디안 상한을 넘어 반각 89도까지 맞추도록 실제 원뿔의 초점 계수를 다시 넣으며, Y는 그림자 규약에 맞춰 음수로 둔다.
		proj.m[0] = 1.f/std::tan(fovYRadians*.5f);
		proj.m[5] = -proj.m[0];
		return proj * view;
	}

	// 좌표나 방향의 세 성분이 모두 유한한지 확인하는 지역 검사다. 유효하지 않은 지역 광원 그림자 시점을 만들지 않게 한다.
	bool IsFinitePosition(const Math::float3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	// 셰이더가 그림자 깊이를 거리와 필터 폭으로 바꾸는 데 필요한 투영 계수만 추출한다. 앞의 두 값은 가로/세로 반배율이다.
	Math::float4 ShadowProjectionParams(const Math::float4x4& matrix,float nearPlane=0,float farPlane=0)
	{
		const float x=.5f*Math::Length(Math::float3(matrix.m[0],matrix.m[4],matrix.m[8]));
		const float y=.5f*Math::Length(Math::float3(matrix.m[1],matrix.m[5],matrix.m[9]));
		if(nearPlane>0 && farPlane>nearPlane) // 유효한 원근 범위가 주어지면 z/w에 원근 깊이 복원 계수를 저장한다.
		{
			const float denominator=nearPlane/farPlane-1.f;
			return {x,y,1.f/denominator,nearPlane/denominator};
		}
		// near/far를 주지 않은 방향광은 직교 투영으로 취급하여 깊이 배율의 역수를 저장한다.
		const float depthScale=Math::Length(Math::float3(matrix.m[2],matrix.m[6],matrix.m[10]));
		return {x,y,depthScale>0?1.f/depthScale:0.f,0};
	}

	// 지역 광원 범위에 비례하는 작은 near를 정해 광원 바로 옆의 차폐물이 고정 near=0.1 때문에 통째로 잘리지 않게 한다.
	bool LocalShadowClipRange(float range, float& nearPlane)
	{
		if(!std::isfinite(range) || range <= 0) return false; // 범위가 유한한 양수가 아니면 그림자 투영 대상에서 제외한다.
		// near는 광원 범위의 0.1%와 0.1 중 작은 값으로 둔다. 아주 작은 range의 곱이 0으로 반올림되거나 near가 range 이상이면 마지막 조건에서 false를 반환한다.
		nearPlane = std::min(.1f, range * .001f);
		return nearPlane > 0 && nearPlane < range;
	}

	// 로컬 AABB의 여덟 꼭짓점을 변환해 다른 좌표계의 AABB에 합친다. 회전·비균일 확대가 있어도 꼭짓점 전체를 포함한다.
	void IncludeTransformedBounds(Math::Bounds3& bounds, const Math::Bounds3& local,
		const Math::float4x4& transform)
	{
		if(!local.valid) return; // 로컬 경계를 모르면 임의 값을 포함시키지 않고 현재 누적 경계를 유지한다.
		for(uint32_t corner = 0; corner < 8; ++corner)
			bounds.Include(Math::TransformPoint(transform, {
				corner & 1 ? local.max.x : local.min.x,
				corner & 2 ? local.max.y : local.min.y,
				corner & 4 ? local.max.z : local.min.z}));
	}

}

// 깊이 결과에 영향을 주는 현재 자료를 직렬화해 마지막 성공 캐시와 비교한다. true이면 기존 아틀라스를 다시 그리지 않아도 되는 후보 상태다.
bool Renderer::Impl::ReuseShadowAtlas(const Scene& scene,ShadowData& shadows,const std::vector<RendererDrawDesc>* draws)
{
    auto& cache=shadowAtlasCache;
    cache.pending=false;cache.reused=false;shadows.dirtyViews.fill(true);
    if(!config.lighting.reuseStaticShadows || (draws && !stockDrawInputs) || !UsesStockGeometry() ||
        !shadows.viewCount || shadows.viewCount>cache.views.size() || !shadowPipeline || !shadowDepthTarget || !shadowMatrixBuffer)
    {
        cache.valid=false;return false;
    }
    auto* destination=&cache.pendingBytes;
    destination->clear();
    const auto append=[&](const auto& value) {
        const auto* first=reinterpret_cast<const uint8_t*>(&value);
        destination->insert(destination->end(),first,first+sizeof(value));
    };
    const auto appendPosition=[&](const Math::float3& value) {append(value.x);append(value.y);append(value.z);};
    append(config.lighting.shadowQuality);append(config.enableFrustumCulling);append(config.enableInstancing);
    append(shadows.viewCount);append(shadows.columns);append(shadows.rows);append(shadows.resolution);
    append(shadowArenaGeneration);
    const bool sameAtlas=cache.valid && cache.target==shadowDepthTarget && !cache.scene.expired() &&
        !cache.scene.owner_before(scene.m_lifetime) && !scene.m_lifetime.owner_before(cache.scene) &&
        cache.bytes==cache.pendingBytes;
    auto& casterBytes=cache.casterBytes;casterBytes.clear();
    cache.casterRanges.assign(scene.GetEntityCount(),{});
    destination=&casterBytes;
    uint32_t previous=UINT32_MAX;
    for(const auto& caster:shadowCasterScratch)
    {
        if(caster.entity==previous)continue;
        previous=caster.entity;
        const auto entity=static_cast<EntityID>(caster.entity);
        const auto& mesh=m_meshes[caster.mesh];
        const auto& material=scene.Materials()[caster.material];
        const auto& state=materialStates[caster.material];
        const size_t first=casterBytes.size();
        append(caster.entity);append(caster.mesh);append(caster.material);
        append(reinterpret_cast<uintptr_t>(mesh.vertexBuffer));
        append(reinterpret_cast<uintptr_t>(mesh.indexBuffer));append(mesh.indexCount);
        append(caster.arena);append(caster.compact);append(caster.coverageFlags);
        append(scene.GetTransform(entity).m);append(material.alphaMode);
        append(material.baseColor.w);append(material.alphaCutoff);append(material.doubleSided);
        append(reinterpret_cast<uintptr_t>(state.textures[0]));
        append(reinterpret_cast<uintptr_t>(Detail::ImageDdsAccess::Identity(material.baseColorTexture)));
        append(material.baseColorTexture.GetWidth());append(material.baseColorTexture.GetHeight());
        append(material.baseColorTexture.GetColorSpace());
        if(draws)
        {
            const auto& draw=(*draws)[caster.entity];
            if(draw.vertexBuffer && !draw.geometryRevision) {cache.valid=false;return false;}
            append(reinterpret_cast<uintptr_t>(draw.vertexBuffer));append(draw.geometryRevision);
            append(draw.hasLocalBounds);
            if(draw.hasLocalBounds)
            {
                append(draw.localBounds.valid);
                if(draw.localBounds.valid) {appendPosition(draw.localBounds.min);appendPosition(draw.localBounds.max);}
            }
        }
        cache.casterRanges[caster.entity]={first,casterBytes.size()-first};
    }
    bool dirty=false;
    for(uint32_t view=0;view<shadows.viewCount;++view)
    {
        auto& entry=cache.views[view];entry.pendingBytes.clear();entry.pendingMeshes.clear();
        destination=&entry.pendingBytes;
        append(shadows.constants.lightViewProjectionMatrix[view].m);
        append(shadows.casterVolumes[view].count);
        for(uint32_t plane=0;plane<shadows.casterVolumes[view].count;++plane)append(shadows.casterVolumes[view].planes[plane]);
        const auto& rect=shadows.constants.atlasRect[view];append(rect.x);append(rect.y);append(rect.z);append(rect.w);
        CollectShadowViewCasters(shadowCasterScratch,shadows,view,config.enableFrustumCulling,shadowViewScratch);
        append(shadowViewScratch.size());
        previous=UINT32_MAX;
        for(uint32_t index:shadowViewScratch)
        {
            const auto& caster=shadowCasterScratch[index];
            append(caster.entity);append(caster.firstIndex);append(caster.indexCount);
            if(previous==caster.entity)continue;
            previous=caster.entity;
            const auto range=cache.casterRanges[caster.entity];
            destination->insert(destination->end(),casterBytes.begin()+range.first,casterBytes.begin()+range.first+range.second);
            entry.pendingMeshes.push_back(m_meshes[caster.mesh].source);
        }
        bool reusable=sameAtlas && entry.bytes==entry.pendingBytes && entry.meshes.size()==entry.pendingMeshes.size();
        for(size_t i=0;reusable && i<entry.meshes.size();++i)
            reusable=!entry.meshes[i].expired() && !entry.meshes[i].owner_before(entry.pendingMeshes[i]) &&
                !entry.pendingMeshes[i].owner_before(entry.meshes[i]);
        shadows.dirtyViews[view]=!reusable;dirty|=!reusable;
    }
    // Bounded LoadOp::Clear is an explicit backend contract; otherwise preserve the full redraw path.
    if(dirty && !device->Supports(RHI::Feature::RenderArea))shadows.dirtyViews.fill(true);
    cache.pending=true;cache.reused=!dirty;
    return cache.reused;
}

void Renderer::Impl::CommitShadowAtlas(const Scene& scene)
{
    auto& cache=shadowAtlasCache;
    if(!cache.pending)return;
    cache.bytes.swap(cache.pendingBytes);
    for(auto& view:cache.views)
    {
        view.bytes.swap(view.pendingBytes);view.meshes.swap(view.pendingMeshes);
    }
    cache.scene=scene.m_lifetime;cache.target=shadowDepthTarget;
    cache.valid=true;cache.pending=false;
}

// 설정별 뷰 예산 → 카메라 구간 → 광원별 그림자 선택 → 아틀라스 크기 → 광원 행렬 순서로 ShadowData를 채운다.
void Renderer::Impl::BuildShadows(ShadowData& state,const Scene& scene,const Camera& camera,
    const std::vector<RendererDrawDesc>* draws)
{
    const auto quality=config.lighting.shadowQuality;
    // 타일 목표 해상도와 전체 아틀라스 예산을 품질에 따라 정한다. 저품질은 한 캐스케이드, 나머지는 네 구간을 기본으로 한다.
    const uint32_t resolution=quality==ShadowQuality::Low?1024:quality==ShadowQuality::High?4096:2048;
    const uint32_t atlasResolution=quality==ShadowQuality::Low?2048:quality==ShadowQuality::High?8192:4096;
    const uint32_t cascadeCount=quality==ShadowQuality::Low?1:4;
    auto& constants=state.constants;auto& viewCount=state.viewCount;auto& columns=state.columns;auto& rows=state.rows;
    // 재사용되는 출력 구조체를 먼저 0/최소값으로 초기화한다. 아래에서 조기 반환해도 이전 프레임 그림자 선택이 남지 않는다.
    constants={};state.casterVolumes={};viewCount=0;columns=rows=state.resolution=1;state.directionalDistance=0;
    constants.cameraView=camera.view;
    // 차폐 탐색 샘플 수·필터 샘플 수·품질별 기본 필터 배율을 기록한다. filterParams.w의 카메라 near는 아래에서 카메라 구간이 유효할 때 채운다.
    constants.filterParams={static_cast<float>(config.lighting.shadowBlockerSamples),
        static_cast<float>(config.lighting.shadowFilterSamples),quality==ShadowQuality::High?2.f:1.f,0};
    constants.softShadowParams={config.lighting.shadowMaxFilterRadius,0,0,0};
    if(!config.lighting.enabled||!config.lighting.direct||!config.lighting.shadows)return; // 조명·직접광·그림자 중 하나라도 꺼져 있으면 빈 그림자 상태로 끝낸다.
    // 직접광 고정 배열과 같은 우선순위 순서를 확보한다. 그중 그림자를 만드는 슬롯만 별도 비트마스크로 고른다.
    const auto directional=SelectActiveLightIndices(scene.DirectionalLights(),static_cast<uint32_t>(std::size(constants.directionalViews)));
    const auto points=SelectActiveLightIndices(scene.PointLights(),static_cast<uint32_t>(std::size(constants.pointViews)));
    const auto spots=SelectActiveLightIndices(scene.SpotLights(),static_cast<uint32_t>(std::size(constants.spotViews)));
    const auto& projection=camera.projection;
    const bool finiteProjection=std::all_of(std::begin(projection.m),std::end(projection.m),
        [](float value){return std::isfinite(value);});
    // 행렬이 유한하고 중심 대칭인 표준 카메라인지 엄격히 검사한다. 비대칭·특수 투영에 표준 near/far 복원식을 무리하게 적용하지 않는다.
    const bool centeredProjection=finiteProjection &&
        projection.m[0]>0 && projection.m[5]>0 &&
        projection.m[1]==0 && projection.m[2]==0 && projection.m[3]==0 &&
        projection.m[4]==0 && projection.m[6]==0 && projection.m[7]==0 &&
        projection.m[8]==0 && projection.m[9]==0 && projection.m[12]==0 && projection.m[13]==0 &&
        std::isfinite(1/projection.m[0]) && std::isfinite(1/projection.m[5]);
    const bool orthographic=centeredProjection && projection.m[11]==0 && projection.m[15]==1;
    const bool perspective=centeredProjection && projection.m[11]==-1 && projection.m[15]==0;
    // 0..1 깊이 규약의 직교/원근 투영 계수에서 near/far를 복원한다. 이후 유효성 검사와 useCameraFit 조건이 이 값을 사용할지 결정한다.
    const float cameraNear=projection.m[14]/projection.m[10];
    const float cameraFar=orthographic?(projection.m[14]-1)/projection.m[10]:projection.m[14]/(projection.m[10]+1);
    const bool validCameraRange=std::isfinite(cameraNear) && std::isfinite(cameraFar) && cameraFar>cameraNear &&
        (orthographic || (perspective && cameraNear>0));
    const bool explicitDistance=config.lighting.shadowDistance>0;
    const float shadowFar=explicitDistance?std::min(cameraFar,config.lighting.shadowDistance):cameraFar;
    // 유효한 원근 카메라의 다중 캐스케이드 또는 명시된 그림자 거리에는 카메라 구간을 맞추고, 그 외에는 장면 경계 맞춤을 유지한다.
    const bool useCameraFit=validCameraRange && ((perspective && cascadeCount>1) || explicitDistance);
    const bool directionalRangeValid=!useCameraFit || shadowFar>cameraNear;
    if(useCameraFit && directionalRangeValid) // 카메라 구간이 실제로 사용할 수 있을 때만 캐스케이드 이력과 near 필터 값을 갱신한다.
    {
        const float aspect=projection.m[5]/projection.m[0];
        if(std::fabs(cameraNear-shadowCascadeNear)>std::max(std::fabs(cameraNear),.0001f)*.0001f || // near/far·종횡비·분할 방식·투영 종류·단계 수가 달라지면 안정화 반경 캐시를 초기화한다.
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
    // 직접광 배열 순서를 유지한 채 우선순위와 예산에 맞는 그림자 슬롯 비트만 켜서 조명 인덱스와 그림자 인덱스의 대응을 보존한다.
    const auto selectShadowSlots=[](const auto& selected,const auto& lights,uint32_t budget,const auto& valid) {
        uint32_t slots=0,used=0;
        for(uint32_t slot=0;slot<selected.size() && used<budget;++slot)
        {
            const auto& light=lights[selected[slot]];
            if(!light.castShadow || !(light.shadowStrength>0) || !(light.intensity>0) || !valid(light))continue; // 그림자를 끄거나 양수 강도가 없거나 위치/범위 검사를 통과하지 못한 광원은 그림자 예산에서 제외한다.
            slots|=1u<<slot;++used;
        }
        return slots;
    };
    const uint32_t directionalShadowSlots=directionalRangeValid?
        selectShadowSlots(directional,scene.DirectionalLights(),config.lighting.maxDirectionalShadowLights,
            [](const auto&){return true;}):0;
    // 점광원은 유한한 위치와 만들 수 있는 near/far 범위를 요구한다. 스폿은 여기에 유한하고 길이가 있는 진행 방향도 필요하다.
    const uint32_t pointShadowSlots=selectShadowSlots(points,scene.PointLights(),config.lighting.maxPointShadowLights,
        [](const auto& light){float nearPlane;return IsFinitePosition(light.position) && LocalShadowClipRange(light.range,nearPlane);});
    const uint32_t spotShadowSlots=selectShadowSlots(spots,scene.SpotLights(),config.lighting.maxSpotShadowLights,
        [](const auto& light){float nearPlane;return IsFinitePosition(light.position) && IsFinitePosition(light.direction) &&
            Math::Length(light.direction)>0 && LocalShadowClipRange(light.range,nearPlane);});
    uint32_t expectedViewCount=0;
    for(uint32_t slot=0;slot<directional.size();++slot)
        if(directionalShadowSlots&(1u<<slot))expectedViewCount+=useCameraFit?cascadeCount:1; // 선택된 방향광마다 카메라 맞춤이면 캐스케이드 수, 아니면 한 뷰를 예약한다.
    for(uint32_t slot=0;slot<points.size();++slot)
        if(pointShadowSlots&(1u<<slot))expectedViewCount+=6; // 선택된 점광원 하나는 전 방향을 덮는 여섯 뷰가 필요하다.
    for(uint32_t slot=0;slot<spots.size();++slot)
        if(spotShadowSlots&(1u<<slot))++expectedViewCount; // 선택된 스폿 하나는 원뿔 한 뷰만 필요하다.
    // 필요 뷰 수의 제곱근을 올림하여 가로 칸 수를 정하고 세로 칸을 맞춘다. 뷰가 없어도 나눗셈용 최소 크기 1을 유지한다.
    columns=std::max(1u,static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(expectedViewCount)))));
    rows=std::max(1u,(expectedViewCount+columns-1)/columns);
    // 직접광·선택된 그림자 수를 유지하며 품질 예산·장치 크기 한도에 맞춰 타일 해상도를 줄이고, 행렬 안정화·깊이 텍스처·viewport에 함께 사용한다.
    const auto atlasLimit=std::min<uint64_t>(atlasResolution,device->GetLimit(RHI::Limit::Texture2DDimension));
    state.resolution=static_cast<uint32_t>(std::min<uint64_t>(resolution,atlasLimit/std::max(columns,rows)));
    Math::Bounds3 bounds;
    // 정적 장면 맞춤은 원본 정점의 정확한 경계를 유지하고 변형 입력만 현재 경계를 사용한다.
    const bool needsSceneBounds=!useCameraFit && directionalShadowSlots!=0;
    if(needsSceneBounds)for(uint32_t i=0;i<scene.GetEntityCount();++i) // 장면 맞춤 방향광이 있을 때만 원본 정점 기반의 정확한 월드 경계를 계산한다.
    {
        const auto entity=static_cast<EntityID>(i);
        const auto mesh=scene.GetEntityMesh(entity);
        const auto flags=scene.GetEntityLighting(entity);
        if(!IsValid(mesh)||ToIndex(mesh)>=scene.Meshes().size()||(!flags.castShadow&&!flags.receiveShadow))continue; // 메시가 없거나 그림자를 만들지도 받지도 않는 엔티티는 관련 경계에서 제외한다.
        if(draws && (*draws)[i].hasLocalBounds && (*draws)[i].localBounds.valid) // 변형 입력이 유효한 현재 경계를 제공하면 정적 원본 정점 경계 대신 그것을 사용한다.
        {
            IncludeTransformedBounds(bounds,(*draws)[i].localBounds,scene.GetTransform(entity));
            continue;
        }
        for(const auto& vertex:(*scene.Meshes()[ToIndex(mesh)]).vertices)
            bounds.Include(Math::TransformPoint(scene.GetTransform(entity),vertex.position));
    }
    // 카메라 구간의 XY 크기와 별개로 광원 깊이는 화면 밖 관련 물체와 현재 변형 경계까지 보수적으로 포함한다.
    if(useCameraFit && directionalShadowSlots)for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto mesh=scene.GetEntityMesh(entity);
        const auto flags=scene.GetEntityLighting(entity);
        if(!IsValid(mesh) || (!flags.castShadow && !flags.receiveShadow) || ToIndex(mesh)>=m_meshes.size())continue; // 그림자와 무관하거나 준비 메시 범위를 벗어나는 엔티티는 깊이 경계 합산에서 건너뛴다.
        const auto& local=draws && (*draws)[i].hasLocalBounds && (*draws)[i].localBounds.valid?
            (*draws)[i].localBounds:m_meshes[ToIndex(mesh)].bounds;
        IncludeTransformedBounds(bounds,local,scene.GetTransform(entity));
    }
    for(uint32_t i=0;i<directional.size();++i)
    {
        const auto& light=scene.DirectionalLights()[directional[i]];
        if(!(directionalShadowSlots&(1u<<i)))continue; // 선택되지 않은 방향광은 초기 0 뷰 정보를 유지한다.
        uint32_t count=1;
        const uint32_t first=viewCount;
        if(useCameraFit) // 카메라 거리 구간별 캐스케이드를 만들고, 아니면 장면 전체를 덮는 한 시점을 만든다.
        {
            const float nearPlane=cameraNear;
            const float farPlane=shadowFar;
            count=cascadeCount;
            auto& splits=constants.directionalSplits[i];
            for(uint32_t c=0;c<count;++c)
            {
                const float fraction=static_cast<float>(c+1u)/static_cast<float>(count);
                // 선형 분할은 거리를 균등하게, 로그 분할은 가까운 쪽을 더 촘촘하게 배분한다. lambda로 둘을 섞으며 마지막 경계는 far로 고정한다.
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
                // 이전 캐스케이드의 끝 10%와 다음 구간을 겹쳐 셰이더의 부드러운 전환에 필요한 영역을 확보한다.
                const float previousStart=c<2?nearPlane:splits[c-2];
                const float sliceNear=c==0?nearPlane:splits[c-1]-.1f*(splits[c-1]-previousStart);
                const float sliceFar=splits[c];
                const float receiverNear=c==0?std::min(0.f,sliceNear):sliceNear;
                const auto center=camera.position+forward*((receiverNear+sliceFar)*.5f);
                // 구간 깊이의 절반과 가장 넓은 면의 반폭·반높이를 세 변으로 하는 벡터 길이를 구해 카메라 구간 전체를 감싸는 구의 반경으로 쓴다. 카메라 회전만으로 이 반경은 변하지 않는다.
                const float halfDepth=(sliceFar-receiverNear)*.5f;
                const float halfWidth=(orthographic?1.f:sliceFar)/projection.m[0];
                const float halfHeight=(orthographic?1.f:sliceFar)/projection.m[5];
                const float radius=std::sqrt(halfDepth*halfDepth+halfWidth*halfWidth+halfHeight*halfHeight);
                // 반경을 1/16 단위로 올림하고 이력 안에서는 줄이지 않아 격자 수축에 따른 그림자 떨림을 막으며, 구조가 바뀌면 앞에서 이력을 초기화한다.
                shadowCascadeRadii[c]=std::max(shadowCascadeRadii[c],std::ceil(radius*16)/16);
                const uint32_t view=viewCount++;
                constants.lightViewProjectionMatrix[view]=
                    ComputeStableCascadeViewProj(light.direction,center,shadowCascadeRadii[c],bounds,state.resolution);
                if(stockDrawInputs && UsesStockGeometry() && config.enableFrustumCulling)
                {
                    const double filterRadius=std::max(double(config.lighting.shadowMaxFilterRadius),
                        double(constants.filterParams.z)+1)+2;
                    const double padding=2*double(shadowCascadeRadii[c])*filterRadius/state.resolution;
                    // Perspective fog also receives shadows between the camera and the first surface near plane.
                    state.casterVolumes[view].Build(camera,receiverNear,sliceFar,light.direction,padding);
                }
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
        // 광원별 첫 뷰 번호·뷰 수·그림자 강도·각반경의 tan을 저장한다. 실제 행렬 배열과 이 메타정보를 셰이더가 함께 읽는다.
        constants.directionalViews[i]={static_cast<float>(first),static_cast<float>(count),light.shadowStrength,radius};
        for(uint32_t view=first;view<viewCount;++view)
            constants.viewProjectionParams[view]=ShadowProjectionParams(constants.lightViewProjectionMatrix[view]);
    }
    for(uint32_t i=0;i<points.size();++i)
    {
        const auto& light=scene.PointLights()[points[i]];
        if(!(pointShadowSlots&(1u<<i)))continue; // 점광원 그림자 예산에 든 슬롯만 여섯 시점을 추가한다.
        const float nearPlane=std::min(.1f,light.range*.001f);
        const auto matrices=ComputePointLightViewProjections(light.position,nearPlane,light.range);
        const float radius=std::isfinite(light.sourceRadius)?std::max(light.sourceRadius,0.f):0.f;
        // 첫 뷰부터 연속된 여섯 타일을 이 점광원에 할당하고, sourceRadius는 부드러운 그림자 계산 입력으로 남긴다.
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
        if(!(spotShadowSlots&(1u<<i)))continue; // 스폿 그림자 예산에 든 슬롯만 한 시점을 추가한다.
        const float nearPlane=std::min(.1f,light.range*.001f);
        const float radius=std::isfinite(light.sourceRadius)?std::max(light.sourceRadius,0.f):0.f;
        constants.spotViews[i]={static_cast<float>(viewCount),1,light.shadowStrength,radius};
        // 직접광 평가와 같은 정렬·제한된 바깥 원뿔 각도를 사용해 그림자 투영 범위를 맞춘다.
        const auto cone=EffectiveSpotCone(light.innerConeRadians,light.outerConeRadians);
        constants.lightViewProjectionMatrix[viewCount]=ComputeSpotLightViewProj(light.position,light.direction,
            std::max(cone.outer*2,.001f),nearPlane,light.range);
        constants.viewProjectionParams[viewCount]=ShadowProjectionParams(constants.lightViewProjectionMatrix[viewCount],nearPlane,light.range);
        ++viewCount;
    }
    // 연속 뷰 번호를 아틀라스의 행/열에 배치하고 정규화된 시작 UV와 타일 크기를 저장한다.
    for(uint32_t i=0;i<viewCount;++i)
        constants.atlasRect[i]={static_cast<float>(i%columns)/columns,static_cast<float>(i/columns)/rows,1.f/columns,1.f/rows};
}
}
