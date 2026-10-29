#include "../Platform/Profiler/CollectorInternal.h"
#include "../Platform/Profiler/RhiProfiler.h"
#include "RendererInternal.h"
#include "SceneAlgorithms.h"
#include "DrawCommands.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/Buffer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace dyf
{
namespace {
// 각 좌표축에 나란한 면으로 물체를 감싸는 상자(AABB)를 로컬 좌표에서 affine 월드 변환으로 옮겨 월드 AABB를 만든다.
// 중심은 행렬로 옮기고 반폭은 행렬 성분의 절댓값으로 합산해 회전·확대된 경계를 포함한다. double로 계산한 뒤 float 저장 경계를 바깥으로 한 단계 넓힌다.
Math::Bounds3 ShadowWorldBounds(const Math::Bounds3& local,const Math::float4x4& world,std::array<double,3>& magnitude)
{
    if(!local.valid || world.m[3]!=0 || world.m[7]!=0 || world.m[11]!=0 || world.m[15]!=1)return {}; // 로컬 경계가 없거나 행렬의 마지막 행이 (0,0,0,1)이 아니면 이 affine 계산을 적용하지 않고 무효 경계를 반환한다. 이후 Outside는 이 경계의 객체를 제외하지 않는다.
    for(float value:world.m)if(!std::isfinite(value))return {}; // 월드 행렬에 NaN/무한대가 있으면 경계 계산을 중단한다.
    const double minimum[]={local.min.x,local.min.y,local.min.z};
    const double maximum[]={local.max.x,local.max.y,local.max.z};
    double center[3],extent[3];
    for(uint32_t axis=0;axis<3;++axis) {
        if(!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || minimum[axis]>maximum[axis])return {}; // 경계값이 비유한 값이거나 최소/최대 순서가 뒤집혔으면 무효 처리한다.
        center[axis]=(minimum[axis]+maximum[axis])*.5;
        extent[axis]=(maximum[axis]-minimum[axis])*.5;
    }
    float lower[3],upper[3];
    for(uint32_t row=0;row<3;++row) {
        double position=world.m[12+row],radius=0;
        // 월드 좌표의 큰 항이 서로 상쇄되는 경우에도 오차 크기 평가를 위해 각 항의 절댓값을 따로 누적한다.
        magnitude[row]=std::abs(position);
        for(uint32_t column=0;column<3;++column) {
            const double coefficient=world.m[column*4+row];
            position+=coefficient*center[column];radius+=std::abs(coefficient)*extent[column];
            magnitude[row]+=std::abs(coefficient)*std::max(std::abs(minimum[column]),std::abs(maximum[column]));
        }
        // float 변환 뒤 nextafter로 최소는 -무한대 쪽, 최대는 +무한대 쪽으로 한 단계 옮긴다. 저장 반올림 때문에 실제 경계가 안쪽으로 좁아지는 것을 막는다.
        lower[row]=std::nextafter(static_cast<float>(position-radius),-std::numeric_limits<float>::infinity());
        upper[row]=std::nextafter(static_cast<float>(position+radius),std::numeric_limits<float>::infinity());
        if(!std::isfinite(lower[row]) || !std::isfinite(upper[row]))return {}; // 확장한 float 경계도 유한하게 표현되지 않으면 무효 경계를 돌려준다.
    }
    return {{lower[0],lower[1],lower[2]},{upper[0],upper[1],upper[2]},true};
}

// 광원 view-projection에서 여섯 평면과 각 계수의 크기를 보관한다. 유효하지 않은 평면은 제거 판정에 사용하지 않는다.
struct ShadowFrustum
{
    std::array<std::array<double,4>,6> planes={},magnitudes={};
    bool valid=false;
    // 클립 공간 평면을 추출한다. near 평면(4번)은 깊이 0 경계를 쓰고 나머지는 w±x/y/z 조합이다.
    explicit ShadowFrustum(const Math::float4x4& matrix)
    {
        for(float value:matrix.m)if(!std::isfinite(value))return; // 광원 행렬이 비유한 값이면 frustum을 valid로 만들지 않는다.
        for(uint32_t plane=0;plane<6;++plane) {
            const uint32_t row=plane/2;
            const double sign=plane%2?-1:1;
            for(uint32_t column=0;column<4;++column) {
                const double a=matrix.m[column*4+3],b=matrix.m[column*4+row];
                planes[plane][column]=plane==4?b:a+sign*b;
                magnitudes[plane][column]=plane==4?std::abs(b):std::abs(a)+std::abs(b);
            }
            if(planes[plane][0]==0 && planes[plane][1]==0 && planes[plane][2]==0)return; // 표면에 수직인 방향(법선)을 나타내는 평면의 앞 세 계수가 모두 0이면 유효한 절두체로 사용하지 않는다.
        }
        valid=true;
    }
    // 각 평면의 계수 부호에 따라 AABB의 min/max를 골라 평면식 값이 가장 큰 점을 검사한다. 이 값마저 -1e-5*max(1,scale)보다 작을 때만 상자 전체가 오차 여유를 넘어 그 평면 밖이라고 판정한다.
    bool Outside(const Math::Bounds3& bounds,const std::array<double,3>& worldMagnitude) const
    {
        if(!valid || !bounds.valid)return false; // 절두체나 경계가 불확실하면 밖이라고 판단하지 않아 잘못된 제거를 피한다.
        const double minimum[]={bounds.min.x,bounds.min.y,bounds.min.z};
        const double maximum[]={bounds.max.x,bounds.max.y,bounds.max.z};
        for(uint32_t plane=0;plane<6;++plane) {
            double distance=planes[plane][3],scale=magnitudes[plane][3];
            for(uint32_t axis=0;axis<3;++axis) {
                distance+=planes[plane][axis]*(planes[plane][axis]>=0?maximum[axis]:minimum[axis]);
                // 셰이더의 light*world 연산에서도 상쇄 전 월드 항의 크기를 유지한다.
                scale+=magnitudes[plane][axis]*worldMagnitude[axis];
            }
            if(distance < -1e-5*std::max(1.0,scale))return true; // 오차 크기에 따른 여유보다 확실하게 평면 밖일 때만 제거한다.
        }
        return false;
    }
};

bool ShadowSortProjection(const Math::float4x4& matrix,const Math::float4& parameters,bool& perspective)
{
    perspective=false;
    for(float value:matrix.m)if(!std::isfinite(value) || std::abs(value)>1e10f)return false;
    // Canonical orthographic light views keep clip w exactly one in either
    // light*(world*position) or (light*world)*position evaluation order.
    if(matrix.m[3]==0 && matrix.m[7]==0 && matrix.m[11]==0 && matrix.m[15]==1)return true;
    // Engine-generated point/spot projections have z = -A*w + B, A <= -1,
    // B < 0. Verify that relation before using clip w as increasing view depth.
    const double a=parameters.z,b=parameters.w;
    if(!std::isfinite(a) || !std::isfinite(b) || a>-1 || b>=0)return false;
    double directionSquared=0;
    for(uint32_t column=0;column<4;++column)
    {
        const double z=matrix.m[column*4+2],w=matrix.m[column*4+3];
        const double constant=column==3?b:0;
        const double expected=-a*w+constant;
        const double scale=std::max({1.0,std::abs(z),std::abs(a*w),std::abs(constant)});
        if(std::abs(z-expected)>1e-5*scale)return false;
        if(column<3)directionSquared+=w*w;
    }
    if(std::abs(directionSquared-1.0)>1e-4)return false;
    perspective=true;
    return true;
}

bool ShadowBatchNearDepth(const Math::Bounds3& bounds,const Math::float4x4& matrix,bool perspective,double& depth)
{
    const double minimum[]={bounds.min.x,bounds.min.y,bounds.min.z};
    const double maximum[]={bounds.max.x,bounds.max.y,bounds.max.z};
    const uint32_t row=perspective?3u:2u;
    depth=matrix.m[12+row];
    for(uint32_t axis=0;axis<3;++axis)
    {
        const double coefficient=matrix.m[axis*4+row];
        depth+=coefficient*(coefficient>=0?minimum[axis]:maximum[axis]);
    }
    return std::isfinite(depth) && std::abs(depth)<=1e32;
}

}

void Renderer::Impl::ShadowCasterVolume::Build(const Camera& camera,float nearPlane,float farPlane,
    Math::float3 toLight,double padding)
{
    count=0;
    if(!std::isfinite(nearPlane) || !std::isfinite(farPlane) || farPlane<=nearPlane ||
        !std::isfinite(padding) || padding<0)return;
    for(float value:camera.view.m)if(!std::isfinite(value))return;
    if(camera.view.m[3]!=0 || camera.view.m[7]!=0 || camera.view.m[11]!=0 || camera.view.m[15]!=1)return;
    const double length=std::sqrt(double(toLight.x)*toLight.x+double(toLight.y)*toLight.y+double(toLight.z)*toLight.z);
    if(!std::isfinite(length) || length<1e-12)return;
    const double direction[]={toLight.x/length,toLight.y/length,toLight.z/length};
    const auto matrix=camera.projection*camera.view;
    std::array<std::array<double,4>,6> receiver={};
    std::array<double,6> slope={};
    for(uint32_t i=0;i<6;++i)
    {
        auto& plane=receiver[i];
        for(uint32_t column=0;column<4;++column)
            plane[column]=i<4 ? double(matrix.m[column*4+3])+(i%2?-1:1)*double(matrix.m[column*4+i/2]) :
                (i==4?-1:1)*double(camera.view.m[column*4+2]);
        if(i>=4)plane[3]+=i==4?-double(nearPlane):double(farPlane);
        const double norm=std::sqrt(plane[0]*plane[0]+plane[1]*plane[1]+plane[2]*plane[2]);
        if(!std::isfinite(norm) || norm<1e-12)return;
        for(auto& value:plane) {value/=norm;if(!std::isfinite(value))return;}
        for(uint32_t axis=0;axis<3;++axis)slope[i]+=plane[axis]*direction[axis];
    }
    const auto append=[&](std::array<double,4> plane) {
        const double norm=std::sqrt(plane[0]*plane[0]+plane[1]*plane[1]+plane[2]*plane[2]);
        if(!(norm>1e-12) || !std::isfinite(norm))return;
        for(auto& value:plane)value/=norm;
        plane[3]+=padding;
        planes[count++]=plane;
    };
    // Extrude receiver half-spaces toward the light; silhouette edges close the volume.
    for(uint32_t i=0;i<6;++i)
    {
        if(slope[i]>1e-8)append(receiver[i]);
        for(uint32_t j=i+1;j<6;++j)
        {
            if(i/2==j/2 || !((slope[i]>1e-8 && slope[j]<-1e-8) ||
                (slope[j]>1e-8 && slope[i]<-1e-8)))continue;
            std::array<double,4> plane;
            for(uint32_t k=0;k<4;++k)plane[k]=receiver[i][k]*std::abs(slope[j])+receiver[j][k]*std::abs(slope[i]);
            append(plane);
        }
    }
}

bool Renderer::Impl::ShadowCasterVolume::Outside(const Math::Bounds3& bounds,const std::array<double,3>& magnitude) const
{
    if(!bounds.valid)return false;
    for(uint32_t i=0;i<count;++i)
    {
        const auto& plane=planes[i];
        const double maximum[]={bounds.max.x,bounds.max.y,bounds.max.z};
        const double minimum[]={bounds.min.x,bounds.min.y,bounds.min.z};
        double distance=plane[3],scale=std::abs(plane[3]);
        for(uint32_t axis=0;axis<3;++axis) {
            distance+=plane[axis]*(plane[axis]>=0?maximum[axis]:minimum[axis]);
            scale+=std::abs(plane[axis])*magnitude[axis];
        }
        if(distance < -1e-5*std::max(1.0,scale))return true;
    }
    return false;
}

bool Renderer::Impl::ShadowInstanceData::Build(const Scene& scene,const ShadowData& shadows,
    const std::vector<ShadowCaster>& casters,bool cull,bool allowSingleDraws)
{
    draws.clear();views.clear();entities.clear();matrices.clear();visible.clear();keys.clear();order.clear();arguments.clear();
    constexpr uint32_t stride=2*sizeof(Math::float4x4);
    if(!cull || casters.empty() || !shadows.viewCount ||
        shadows.viewCount>std::size(shadows.constants.lightViewProjectionMatrix) ||
        uint64_t(casters.size())*shadows.viewCount>UINT32_MAX/stride)return false;
    keys.reserve(casters.size());order.reserve(casters.size());
    for(const auto& caster:casters)
    {
        if(!caster.arena || !caster.sortSafe || caster.entity>=scene.GetEntityCount() ||
            caster.material>=scene.Materials().size())return false;
        const auto& material=scene.Materials()[caster.material];
        const bool mask=material.alphaMode==MaterialAlphaMode::Mask;
        uint32_t cutoff=0,alpha=0;
        if(mask)
        {
            if(!std::isfinite(material.alphaCutoff) || !std::isfinite(material.baseColor.w))return false;
            std::memcpy(&cutoff,&material.alphaCutoff,sizeof(cutoff));
            std::memcpy(&alpha,&material.baseColor.w,sizeof(alpha));
        }
        const uint32_t coverage=(mask?1u:0u) | (material.doubleSided?2u:0u) |
            (caster.mirrored?4u:0u) | (mask && (caster.coverageFlags&1u)?8u:0u);
        keys.emplace_back(caster.mesh,reinterpret_cast<uintptr_t>(caster.pipeline),
            reinterpret_cast<uintptr_t>(caster.set),coverage,cutoff,alpha,caster.firstIndex,caster.indexCount);
    }
    views.resize(shadows.viewCount);visible.reserve(casters.size());
    for(uint32_t i=0;i<casters.size();++i)order.push_back(i);
    std::sort(order.begin(),order.end(),[&](uint32_t a,uint32_t b) {
        return keys[a]!=keys[b] ? keys[a]<keys[b] : a<b;
    });
    for(uint32_t view=0;view<shadows.viewCount;++view)
    {
        const auto& projection=shadows.constants.lightViewProjectionMatrix[view];
        bool perspective=false;
        if(!ShadowSortProjection(projection,shadows.constants.viewProjectionParams[view],perspective))return false;
        const ShadowFrustum frustum(projection);
        visible.clear();
        for(uint32_t i:order)
            if(!frustum.Outside(casters[i].worldBounds.bounds,casters[i].worldBounds.magnitude) &&
                !shadows.casterVolumes[view].Outside(casters[i].worldBounds.bounds,casters[i].worldBounds.magnitude))visible.push_back(i);
        const uint32_t firstDraw=static_cast<uint32_t>(draws.size());
        for(size_t first=0;first<visible.size();)
        {
            const uint32_t representative=visible[first];
            const uint32_t firstInstance=static_cast<uint32_t>(entities.size());
            double depth=std::numeric_limits<double>::infinity();
            size_t end=first;
            while(end<visible.size() && keys[visible[end]]==keys[representative])
            {
                const auto& caster=casters[visible[end]];double nearDepth=0;
                if(!ShadowBatchNearDepth(caster.worldBounds.bounds,projection,perspective,nearDepth))return false;
                depth=std::min(depth,nearDepth);
                entities.push_back(caster.entity);
                matrices.push_back(scene.GetTransform(static_cast<EntityID>(caster.entity)));
                matrices.push_back({}); // Preserve the canonical two-matrix instance stride.
                ++end;
            }
            draws.push_back({representative,firstInstance,static_cast<uint32_t>(end-first),depth});
            first=end;
        }
        auto& range=views[view];range={firstDraw,static_cast<uint32_t>(draws.size()-firstDraw)};
        // Keep early depth rejection useful after grouping nonconsecutive entities.
        std::sort(draws.begin()+firstDraw,draws.end(),[](const Draw& a,const Draw& b) {
            return a.depth<b.depth || (a.depth==b.depth && a.caster<b.caster);
        });
    }
    return !draws.empty() && (allowSingleDraws || draws.size()<entities.size());
}

void Renderer::Impl::CollectShadowViewCasters(const std::vector<ShadowCaster>& casters,
    const ShadowData& shadows,uint32_t view,bool cull,std::vector<uint32_t>& visible)
{
    visible.clear();visible.reserve(casters.size());
    const ShadowFrustum frustum(shadows.constants.lightViewProjectionMatrix[view]);
    for(uint32_t i=0;i<casters.size();++i)
    {
        const auto& bounds=casters[i].worldBounds;
        if(!cull || (!frustum.Outside(bounds.bounds,bounds.magnitude) &&
            !shadows.casterVolumes[view].Outside(bounds.bounds,bounds.magnitude)))visible.push_back(i);
    }
}

void Renderer::Impl::PrepareShadowCasters(const Scene& scene,const std::vector<RendererDrawDesc>* draws)
{
    const bool stock=stockDrawInputs,instanced=UsesInstanceStream(),cull=stock && config.enableFrustumCulling;
    auto& casters=shadowCasterScratch;
    casters.clear();
    casters.reserve(scene.GetEntityCount());
    // Validate and classify casters once; all atlas views share these inputs.
    for(uint32_t i=0;i<scene.GetEntityCount();++i) {
        const auto entity=static_cast<EntityID>(i);
        if(!scene.GetEntityLighting(entity).castShadow)continue;
        const auto meshId=scene.GetEntityMesh(entity);
        const auto materialId=scene.GetEntityMaterial(entity);
        if(!IsValid(meshId) || !IsValid(materialId))continue;
        const uint32_t meshIndex=ToIndex(meshId),materialIndex=ToIndex(materialId);
        if(meshIndex>=m_meshes.size() || materialIndex>=materialStates.size())continue;
        const auto& mesh=m_meshes[meshIndex];
        if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount)continue;
        const auto& material=scene.Materials()[materialIndex];
        // A binary shadow map cannot represent translucent coverage.
        if(shadowFragmentShader && UsesBlendedRendering(material))continue;
        ShadowCaster caster;
        caster.entity=i;caster.mesh=meshIndex;caster.material=materialIndex;
        caster.indexCount=mesh.indexCount;
        caster.coverageFlags=materialStates[materialIndex].textureFlags&1u;
        const auto vertexBuffer=draws && (*draws)[i].vertexBuffer ? (*draws)[i].vertexBuffer : mesh.vertexBuffer;
        const bool originalVertices=stock && vertexBuffer==mesh.vertexBuffer;
        caster.arena=originalVertices && shadowArenaVertices && shadowArenaIndices && shadowArenaGeneration &&
            mesh.shadowArena.generation==shadowArenaGeneration && mesh.shadowVertexReady && mesh.shadowIndexReady;
        caster.compact=originalVertices && (caster.arena || (mesh.shadowVertexBuffer && mesh.shadowVertexReady &&
            mesh.shadowIndexBuffer && mesh.shadowIndexReady));
        caster.batch=static_cast<uint32_t>(casters.size());
        const auto& transform=scene.GetTransform(entity);
        caster.mirrored=Math::Determinant3x3(transform)<0;
        if(cull)
            caster.worldBounds.bounds=ShadowWorldBounds(draws && (*draws)[i].hasLocalBounds ? (*draws)[i].localBounds :
                mesh.bounds,transform,caster.worldBounds.magnitude);
        caster.sortSafe=caster.arena && mesh.shadowArena.sortSafeInputs && caster.worldBounds.bounds.valid;
        if(caster.sortSafe)
        {
            // Bound raw matrix inputs as well as transformed magnitudes: GLSL/Metal
            // can multiply light*world before position, so cancellation alone is insufficient.
            for(float value:transform.m)
                if(!std::isfinite(value) || std::abs(value)>1e10f)caster.sortSafe=false;
            for(double value:caster.worldBounds.magnitude)
                if(!std::isfinite(value) || value>1e21)caster.sortSafe=false;
        }
        if(stock && instanced && !casters.empty()) {
            const auto& previous=casters.back();
            const auto& previousMaterial=scene.Materials()[previous.material];
            // The canonical instance stream still requires consecutive entity indices.
            bool same=previous.entity+1==i && previous.mesh==meshIndex && previous.compact==caster.compact &&
                previous.firstIndex==caster.firstIndex && previous.indexCount==caster.indexCount &&
                previous.arena==caster.arena &&
                (!draws || (*draws)[previous.entity].vertexBuffer==(*draws)[i].vertexBuffer) &&
                !UsesBlendedRendering(material) &&
                previousMaterial.doubleSided==material.doubleSided && previous.mirrored==caster.mirrored &&
                (previousMaterial.alphaMode==MaterialAlphaMode::Mask)==(material.alphaMode==MaterialAlphaMode::Mask);
            if(same && material.alphaMode==MaterialAlphaMode::Mask)
                same=materialStates[previous.material].textures[0]==materialStates[materialIndex].textures[0] &&
                    (materialStates[previous.material].textureFlags&1u)==(materialStates[materialIndex].textureFlags&1u) &&
                    previousMaterial.baseColor.w==material.baseColor.w && previousMaterial.alphaCutoff==material.alphaCutoff;
            if(same)caster.batch=previous.batch;
        }
        casters.push_back(caster);
    }
}

bool Renderer::Impl::PrepareShadowPassResources(const Scene& scene,const ShadowData& shadows,
    RHI::ICommandList& uploads,const std::vector<RendererDrawDesc>* draws)
{
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
    Platform::CpuScope cpuScope(Platform::LiveStage::CpuPrepare,"Shadow resources");
#endif
    const bool stock=stockDrawInputs,instanced=UsesInstanceStream(),perEntity=draws && !stock;
    const bool cull=stock && config.enableFrustumCulling;
    const bool canIndirect=!Platform::IsLiveCollectorEnabled() && device->Supports(RHI::Feature::IndirectDraw) &&
        device->Supports(RHI::Feature::IndirectFirstInstance);
    shadowInstancesActive=false;shadowIndirectActive=false;
    auto& casters=shadowCasterScratch;
    const auto shadowPipelineFor=[&](const MaterialDesc& material,bool mirrored,bool compact=false) {
        if(!stock)return UsesStockGeometry() ? shadowMaterialPipelines[2] : shadowPipeline;
        const uint32_t variant=(material.doubleSided?1u:0u) |
            (material.alphaMode==MaterialAlphaMode::Mask?2u:0u) | (mirrored?4u:0u);
        if(compact)return compactShadowPipelines[variant];
        return variant ? shadowMaterialPipelines[variant-1] : shadowPipeline;
    };
	auto& shadowSets=shadowSetScratch;
	{
		// 사용자 자원 입력이면 객체별, fragment가 있으면 재질별, 깊이 정점 전용이면 공통 한 세트로 구성한다.
		shadowSets.assign(perEntity ? scene.GetEntityCount() : shadowFragmentShader ? materialStates.size() : 1, nullptr);
		for(uint32_t i = 0; i < shadowSets.size(); ++i)
		{
			const auto materialIndex=perEntity ? ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(i))) : i;
			if(materialIndex>=materialStates.size()) continue; // 범위 밖 재질에 대해 세트를 만들지 않는다.
			const auto& material=scene.Materials()[materialIndex];
			// Blend·투과 표면의 연속 투과율은 이진 깊이 그림자로 표현하지 않는다.
			if(shadowFragmentShader && UsesBlendedRendering(material)) continue;
			auto& bindings=shadowBindingScratch;bindings = {{3u,
				0, shadowMatrixBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(RendererShadowConstants)), {}}};
			if(shadowFragmentShader && (!stock || material.alphaMode==MaterialAlphaMode::Mask)) // 알파 Mask 또는 비표준 커버리지 경로에만 기본색 텍스처를 연결한다.
			{
				bindings.push_back({0u,0,nullptr,materialStates[materialIndex].textures[0],0,0,{}});
			}
			if(perEntity) // 객체별 사용자 정점 자원을 그림자 기본 바인딩 뒤에 추가한다.
			{
				const auto& extra = (*draws)[i].vertexResources;
				bindings.insert(bindings.end(), extra.begin(), extra.end());
			}
			shadowSets[i] = CachedResourceSet(shadowPipelineFor(material,false,stock),bindings,this->shadowSets);
			if(!shadowSets[i]) // 그림자 자원 세트를 만들지 못하면 패스 기록을 실패 처리한다.
			{
				return false;
			}
		}
        // Resolve immutable bindings once rather than repeating the mirrored
        // pipeline lookup and binding copy in every atlas view.
        for(auto& caster:casters)
        {
            const auto& material=scene.Materials()[caster.material];
            caster.pipeline=shadowPipelineFor(material,caster.mirrored,caster.compact);
            caster.set=shadowSets[perEntity ? caster.entity : shadowFragmentShader ? caster.material : 0];
            if(caster.set && caster.set->GetPipeline()!=caster.pipeline)
            {
                shadowBindingScratch.assign(caster.set->GetBindings(),caster.set->GetBindings()+caster.set->GetBindingCount());
                caster.set=CachedResourceSet(caster.pipeline,shadowBindingScratch,this->shadowSets);
            }
            if(!caster.set)return false;
        }
	}
    if(!stock || !instanced || !UsesStockGeometry() ||
        !shadowInstances.Build(scene,shadows,casters,cull,canIndirect))return true;
    constexpr uint32_t stride=2*sizeof(Math::float4x4);
    const auto bytes=static_cast<uint32_t>(shadowInstances.matrices.size()*sizeof(Math::float4x4));
    if(!shadowInstanceBuffer || shadowInstanceBuffer->GetDesc().size<bytes)
    {
        const uint64_t capacity=std::min<uint64_t>(UINT32_MAX/stride*stride,
            std::max<uint64_t>(bytes,shadowInstanceBuffer ? uint64_t(shadowInstanceBuffer->GetDesc().size)*2 : stride*64));
        // The frame transaction retains the old handle until the draw submission succeeds.
        const auto replacement=device->CreateBuffer({static_cast<uint32_t>(capacity),stride,
            RHI::BufferUsage::Vertex,RHI::ResourceState::CopyDestination});
        if(!replacement)return true; // Optional batching keeps the existing instance path on allocation failure.
        shadowInstanceBuffer=replacement;
        shadowInstanceReady=false;
    }
    if(!shadowInstanceBuffer)return false;
    shadowInstancesActive=true;
    const bool sameInstances=config.enableIncrementalTransformUploads && shadowInstanceReady &&
        shadowInstanceBytes.size()==shadowInstances.matrices.size() &&
        std::memcmp(shadowInstanceBytes.data(),shadowInstances.matrices.data(),bytes)==0;
    if(!sameInstances)
    {
        if(shadowInstanceReady)
        {
            const RHI::ResourceBarrierDesc before={shadowInstanceBuffer,nullptr,RHI::ResourceState::VertexBuffer,
                RHI::ResourceState::CopyDestination,{}};
            uploads.ResourceBarrier(&before,1);
        }
        if(!RecordBufferUpload(device,uploads,shadowInstanceBuffer,shadowInstances.matrices.data(),bytes,
            RHI::ResourceState::VertexBuffer))return false;
    }
    // Preserve per-batch live links on the existing path; stock opaque/MASK runs share inline coverage constants.
    if(!canIndirect)return true;
    auto& arguments=shadowInstances.arguments;arguments.reserve(shadowInstances.draws.size());
    for(const auto& draw:shadowInstances.draws)
    {
        const auto& caster=casters[draw.caster];const auto& range=m_meshes[caster.mesh].shadowArena;
        const uint32_t indexSize=range.indexFormat==RHI::Format::R16_UINT?2u:4u;
        arguments.push_back({caster.indexCount,draw.count,range.indexByteOffset/indexSize+caster.firstIndex,
            static_cast<int32_t>(range.baseVertex),draw.first});
    }
    const auto argumentBytes=static_cast<uint32_t>(arguments.size()*sizeof(arguments.front()));
    if(!shadowArgumentsBuffer || shadowArgumentsBuffer->GetDesc().size<argumentBytes)
    {
        const auto replacement=device->CreateBuffer({argumentBytes,sizeof(RHI::DrawIndexedIndirectArguments),
            RHI::BufferUsage::Indirect,RHI::ResourceState::CopyDestination});
        if(!replacement)return true;
        shadowArgumentsBuffer=replacement;shadowArgumentsReady=false;
    }
    shadowIndirectActive=true;
    if(shadowArgumentsReady && shadowArgumentBytes.size()==arguments.size() &&
        std::memcmp(shadowArgumentBytes.data(),arguments.data(),argumentBytes)==0)return true;
    if(shadowArgumentsReady)
    {
        const RHI::ResourceBarrierDesc before={shadowArgumentsBuffer,nullptr,RHI::ResourceState::IndirectArgument,
            RHI::ResourceState::CopyDestination,{}};
        uploads.ResourceBarrier(&before,1);
    }
    return RecordBufferUpload(device,uploads,shadowArgumentsBuffer,arguments.data(),argumentBytes,
        RHI::ResourceState::IndirectArgument);
}

bool Renderer::Impl::RecordShadowPass(const Scene& scene,const Camera& camera,const ShadowData& shadows,
    RHI::ICommandList& commands,const std::vector<RendererDrawDesc>* draws)
{
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
    Platform::CpuScope cpuScope(Platform::LiveStage::Record,"RendererShadow");
#endif
    Platform::FrameProfiler::GpuScope liveGpu(m_profiler,commands,Platform::LiveGpuScope::RendererShadow,liveId);
    Platform::LiveId sceneBatch=0,instanceId=0;
    const bool stock=stockDrawInputs;
    const bool instanced=UsesInstanceStream();
    const bool baseInstance=stock && instanced;
    const bool cull=stock && config.enableFrustumCulling;
    const bool packed=shadowInstancesActive;
    const auto instanceSource=packed ? shadowInstanceBuffer : instanceBuffer;
    const auto& casters=shadowCasterScratch;
#ifdef DY_RENDERDOC_ENABLED
    commands.BeginDebugEvent("Shadow");
#endif
	const auto viewProjection = camera.projection * camera.view;
	{
		RHI::DepthStencilAttachment depth;
		depth.texture = shadowDepthTarget;
		depth.state = RHI::ResourceState::DepthWrite;
		depth.depthLoadOp = RHI::LoadOp::Clear;
		depth.depthStoreOp = RHI::StoreOp::Store;
		commands.BeginRendering({nullptr, 0, &depth});
		RHI::PipelineHandle boundPipeline=nullptr;
		RHI::ResourceSetHandle boundSet=nullptr;
        RHI::BufferHandle boundVertices=nullptr,boundIndices=nullptr;
        RHI::Format boundIndexFormat=RHI::Format::Unknown;
		// 그림자 뷰마다 아틀라스 타일 viewport/scissor를 설정한다. 같은 객체가 여러 뷰에 그려지면 통계에도 각 뷰 작업이 따로 누적된다.
		for(uint32_t view = 0; view < shadows.viewCount; ++view)
		{
            Platform::LiveId passBatch=0;
			bool inlineConstantsValid=false;
            bool instanceStreamBound=false;
            // 광원 절두체 밖임을 보수적으로 확인한 객체만 이번 뷰 목록에서 제거한다.
            auto& visible=shadowViewScratch;
            if(cull && !packed)CollectShadowViewCasters(casters,shadows,view,true,visible);
            const auto packedRange=packed ? shadowInstances.views[view] : ShadowInstanceData::View{};
            const auto casterAt=[&](uint32_t index) -> const ShadowCaster& {
                if(packed)return casters[shadowInstances.draws[packedRange.first+index].caster];
                return casters[cull ? visible[index] : index];
            };
            const auto visibleCount=packed ? packedRange.count : cull ? visible.size() : casters.size();
            bool perspectiveSort=false;
            if(!packed && stock && cull && visibleCount>1 && UsesStockGeometry() &&
                ShadowSortProjection(shadows.constants.lightViewProjectionMatrix[view],
                    shadows.constants.viewProjectionParams[view],perspectiveSort))
            {
                auto& depths=shadowBatchDepthScratch;
                depths.assign(casters.size(),std::numeric_limits<double>::infinity());
                bool safe=true;
                for(uint32_t index:visible)
                {
                    const auto& caster=casters[index];double depth=0;
                    if(!caster.sortSafe || !ShadowBatchNearDepth(caster.worldBounds.bounds,
                        shadows.constants.lightViewProjectionMatrix[view],perspectiveSort,depth))
                    {
                        safe=false;break;
                    }
                    depths[caster.batch]=std::min(depths[caster.batch],depth);
                }
                if(safe)
                {
                    // All members keep the same batch key. Original caster indices
                    // provide a stable tie break without a temporary sort allocation.
                    // Contiguous entity runs and every draw's primitive order survive.
                    std::sort(visible.begin(),visible.end(),[&](uint32_t a,uint32_t b) {
                        const double first=depths[casters[a].batch],second=depths[casters[b].batch];
                        return first<second || (first==second && a<b);
                    });
                    if(!shadowBatchSortReported && !std::is_sorted(visible.begin(),visible.end()))
                    {
                        std::fprintf(stderr,"[Renderer] Shadow batch depth ordering selected: %s view=%u casters=%zu; finite bounded inputs, unchanged instance runs.\n",
                            perspectiveSort?"perspective":"orthographic",view,visible.size());
                        shadowBatchSortReported=true;
                    }
                }
            }
			const uint32_t x = (view % shadows.columns) * shadows.resolution;
			const uint32_t y = (view / shadows.columns) * shadows.resolution;
			commands.SetViewport({static_cast<float>(x), static_cast<float>(y),
				static_cast<float>(shadows.resolution), static_cast<float>(shadows.resolution), 0, 1});
			commands.SetScissor({static_cast<int32_t>(x), static_cast<int32_t>(y), shadows.resolution, shadows.resolution});
			for(uint32_t drawIndex=0;drawIndex<visibleCount;)
			{
				const auto& caster=casterAt(drawIndex);
				const uint32_t entityIndex=caster.entity;
				const auto entity = static_cast<EntityID>(entityIndex);
				const auto& lighting = scene.GetEntityLighting(entity);
				// Packed draws already group compatible entities; the fallback still requires consecutive entity rows.
				const uint32_t materialIndex=caster.material;
				const auto& mesh = m_meshes[caster.mesh];
				const auto& material=scene.Materials()[materialIndex];
                const auto* packedDraw=packed ? &shadowInstances.draws[packedRange.first+drawIndex] : nullptr;
				uint32_t count=packedDraw ? packedDraw->count : 1;
                while(!packed && drawIndex+count<visibleCount) {
                    const auto& next=casterAt(drawIndex+count);
                    if(next.batch!=caster.batch || next.entity!=entityIndex+count)break;
                    ++count;
                }
                const uint32_t firstInstance=packedDraw ? packedDraw->first : entityIndex;
        // 배치 ID에 Renderer→Scene→그림자 view→객체 범위를 연결한다. 메인 패스와 다른 pass ID를 사용해 같은 객체의 작업을 섞지 않는다.
        auto batchContext=Platform::GetLiveContext();
        if(Platform::IsLiveCollectorEnabled()) // 수집 중이면 그림자 배치의 범위·제한된 이유 분류와 원본 객체 링크를 남긴다.
        {
            if(!sceneBatch)
            {
                sceneBatch=Platform::GetLiveBatchId(liveId,scene.m_liveId);
                instanceId=Platform::FindLiveObject(instanceSource);
            }
            if(!passBatch)passBatch=Platform::GetLiveBatchId(sceneBatch,100+view);
            batchContext.batch=Platform::GetLiveBatchId(passBatch,(uint64_t(entityIndex)<<32)|count);
            batchContext.object=entityIndex<scene.m_liveEntities.size()?scene.m_liveEntities[entityIndex]:scene.m_liveId;
            Platform::LiveEvent batch;batch.id=batchContext.batch;batch.context=batchContext;
            batch.kind=Platform::LiveEventKind::Batch;batch.stage=Platform::LiveStage::Data;
            batch.first=firstInstance;batch.elements=count;batch.stride=2*sizeof(Math::float4x4);
            batch.bytes=uint64_t(count)*batch.stride;batch.resource=instanceId;
            // 이 그림자 배치의 reason은 사용자 기하·인스턴싱 OFF·그 밖(None)만 구분한다. 위에서 실제로 묶기를 끝낸 메시·재질·절두체 조건은 각각 기록하지 않는다.
            batch.reason=!stock?Platform::LiveSplitReason::CustomGeometry:!instanced?Platform::LiveSplitReason::InstancingDisabled:Platform::LiveSplitReason::None;
            Platform::RecordLiveEvent(batch);
            for(uint32_t i=0;i<count;++i)
            {
                const auto original=packed ? shadowInstances.entities[firstInstance+i] : entityIndex+i;
                if(original<scene.m_liveEntities.size())
                    RHI::ProfilerAccess::RecordLink(commands,scene.m_liveEntities[original],batch.id);
            }
        }
        Platform::LiveContextScope linkedBatch(batchContext);
				const auto* input = draws ? &(*draws)[entityIndex] : nullptr;
				const auto selectedPipeline=caster.pipeline;
				if(selectedPipeline!=boundPipeline) // 재질 또는 반전으로 파이프라인이 바뀌면 다시 바인딩하고 세트 기억을 비운다.
				{
					commands.BindGraphicsPipeline(selectedPipeline);boundPipeline=selectedPipeline;boundSet=nullptr;
                    inlineConstantsValid=false;
				}
				const auto selectedSet=caster.set;
				if(selectedSet!=boundSet) {commands.BindResourceSet(selectedSet);boundSet=selectedSet;} // 같은 자원 세트의 반복 바인딩을 생략한다.
				const auto vertexBuffer=input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer;
                const auto selectedVertices=caster.arena ? shadowArenaVertices :
                    caster.compact ? mesh.shadowVertexBuffer : vertexBuffer;
                if(!caster.arena || selectedVertices!=boundVertices)
                {
                    commands.BindVertexBuffer(0,selectedVertices,0);
                    boundVertices=selectedVertices;
                }
                if(instanced)
                {
                    // Stock shaders consume only instance attributes: offset zero
                    // plus firstInstance selects either packed rows or canonical entity rows.
                    if(!baseInstance)commands.BindVertexBuffer(1,instanceSource,firstInstance*2*sizeof(Math::float4x4));
                    else if(!instanceStreamBound)
                    {
                        commands.BindVertexBuffer(1,instanceSource,0);
                        instanceStreamBound=true;
                    }
                }
                const auto selectedIndices=caster.arena ? shadowArenaIndices :
                    caster.compact ? mesh.shadowIndexBuffer : mesh.indexBuffer;
                const auto indexFormat=caster.arena ? mesh.shadowArena.indexFormat :
                    caster.compact ? mesh.shadowIndexFormat : RHI::Format::R32_UINT;
                if(!caster.arena || selectedIndices!=boundIndices || indexFormat!=boundIndexFormat)
                {
                    commands.BindIndexBuffer(selectedIndices,indexFormat,0);
                    boundIndices=selectedIndices;boundIndexFormat=indexFormat;
                }
                // Instanced opaque stock shadows only read the view index here;
                // their model comes from the instance stream and they have no FS.
                const bool reuseConstants=stock && instanced && material.alphaMode==MaterialAlphaMode::Opaque;
                if(!reuseConstants || !inlineConstantsValid)
                {
                    const uint32_t flags=materialStates[materialIndex].textureFlags | (lighting.receiveShadow ? 32u : 0u);
                    auto constants=MakeDrawConstants(viewProjection,material,scene.GetTransform(entity),flags);
                    // 그림자 셰이더가 광원 행렬을 고르도록 draw 상수 슬롯에 view 번호를 넣는다.
                    constants.padding2=view;
                    // Non-stock vertex paths use the common unculled pipeline;
                    // keep its coverage fragment's visible side consistent with mirroring.
                    if(!stock && shadowFragmentShader && caster.mirrored)constants.padding1|=32u;
                    commands.SetInlineConstants(0,sizeof(constants),&constants);
                    if(input && !input->inlineConstants.empty()) // 사용자 추가 상수가 있으면 기본 draw 상수 뒤에 이어 기록한다.
                        commands.SetInlineConstants(sizeof(constants),static_cast<uint32_t>(input->inlineConstants.size()),input->inlineConstants.data());
                    inlineConstantsValid=true;
                }
                uint32_t run=1;
                if(shadowIndirectActive && packed)
                {
                    while(drawIndex+run<visibleCount)
                    {
                        const auto& next=casterAt(drawIndex+run);
                        if(!next.arena || next.pipeline!=caster.pipeline || next.set!=caster.set ||
                            m_meshes[next.mesh].shadowArena.indexFormat!=indexFormat ||
                            (material.alphaMode==MaterialAlphaMode::Mask && next.material!=caster.material))break;
                        ++run;
                    }
                }
                const uint32_t firstIndex=caster.firstIndex+(caster.arena ? mesh.shadowArena.indexByteOffset/
                    (indexFormat==RHI::Format::R16_UINT?2u:4u) : 0);
                const int32_t baseVertex=caster.arena ? static_cast<int32_t>(mesh.shadowArena.baseVertex) : 0;
                if(shadowIndirectActive && packed)
                    commands.DrawIndexedIndirect(shadowArgumentsBuffer,(packedRange.first+drawIndex)*sizeof(RHI::DrawIndexedIndirectArguments),run);
                else commands.DrawIndexedInstanced(caster.indexCount,count,firstIndex,baseVertex,baseInstance?firstInstance:0);
                if(caster.arena && !shadowArenaReported)
                {
                    std::fprintf(stderr,"[Renderer] Shared shadow arena selected: generation=%llu mixed R16/R32 index ranges; %s, cache-optimized triangle order.\n",
                        static_cast<unsigned long long>(shadowArenaGeneration),packed?"packed instance batches":"entity instance runs");
                    shadowArenaReported=true;
                }
				++recordingStatistics.shadowDrawCount;
                for(uint32_t i=0;i<run;++i)
                {
                    const auto instances=packed ? shadowInstances.draws[packedRange.first+drawIndex+i].count : count;
                    recordingStatistics.shadowInstanceCount+=instances;
                    recordingStatistics.shadowTriangleCount+=uint64_t(casterAt(drawIndex+i).indexCount/3)*instances;
                }
				// 배치에 포함한 객체는 다음 draw에서 중복 기록하지 않는다.
				drawIndex+=packed ? run : count;
			}
		}
		commands.EndRendering();

	}
#ifdef DY_RENDERDOC_ENABLED // RenderDoc 빌드에서 열린 Shadow 디버그 이벤트를 닫는다.
	commands.EndDebugEvent();
#endif
	return true;
}
}
