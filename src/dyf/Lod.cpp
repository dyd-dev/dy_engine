#include "dyf/Mesh.h"
#include "RendererInternal.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include <meshoptimizer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>

namespace dyf
{
std::vector<MeshLod> BuildMeshLods(const MeshData& mesh,uint32_t maxLevels)
{
    std::vector<MeshLod> result;
    const auto vertexCount=mesh.vertices.size();
    if(!maxLevels || vertexCount<3 || vertexCount>UINT32_MAX) return result;
    std::vector<uint32_t> sequential;
    if(mesh.indices.empty())
    {
        sequential.resize(vertexCount);
        for(uint32_t i=0;i<vertexCount;++i) sequential[i]=i;
    }
    const auto& original=mesh.indices.empty() ? sequential : mesh.indices;
    if(original.size()<768 || original.size()%3 || original.size()>UINT32_MAX/sizeof(uint32_t)) return result;
    for(uint32_t index:original) if(index>=vertexCount) return result;
    for(const auto& v:mesh.vertices)
        for(float value:{v.position.x,v.position.y,v.position.z,v.normal.x,v.normal.y,v.normal.z,v.uv.x,v.uv.y})
            if(!std::isfinite(value)) return result;
    // FBX triangle corners may be separate indices even when their attributes
    // are identical. Restore that connectivity only for simplification. Compare
    // actual components, never float3/Vertex padding or position alone.
    const auto& first=mesh.vertices.front();
    const meshopt_Stream streams[]={
        {&first.position.x,3*sizeof(float),sizeof(Vertex)},
        {&first.normal.x,3*sizeof(float),sizeof(Vertex)},
        {&first.uv.x,2*sizeof(float),sizeof(Vertex)},
        {&first.tangent.x,4*sizeof(float),sizeof(Vertex)},
        {&first.color.x,4*sizeof(float),sizeof(Vertex)}
    };
    std::vector<uint32_t> remap(vertexCount);
    const size_t compactCount=meshopt_generateVertexRemapMulti(remap.data(),original.data(),original.size(),
        vertexCount,streams,std::size(streams));
    if(compactCount<3) return result;
    std::vector<uint32_t> representatives(compactCount,UINT32_MAX);
    std::vector<std::array<float,3>> positions(compactCount);
    std::vector<std::array<float,5>> attributes(compactCount);
    for(uint32_t index:original)
    {
        const auto compact=remap[index];
        if(representatives[compact]!=UINT32_MAX) continue;
        representatives[compact]=index;
        const auto& v=mesh.vertices[index];
        positions[compact]={v.position.x,v.position.y,v.position.z};
        attributes[compact]={v.normal.x,v.normal.y,v.normal.z,v.uv.x,v.uv.y};
    }
    std::vector<uint32_t> current(original.size());
    meshopt_remapIndexBuffer(current.data(),original.data(),original.size(),remap.data());
    std::vector<uint32_t>().swap(remap);
    const float scale=meshopt_simplifyScale(positions.front().data(),compactCount,sizeof(positions.front()));
    if(!std::isfinite(scale) || scale<=0) return result;
    // Preserve both shading and UV seams. Strict topology plus locked open
    // borders prevents separate material parts from opening cracks.
    const float weights[]={0.5f,0.5f,0.5f,0.5f,0.5f};
    result.reserve(std::min(maxLevels,3u));
    float cumulativeError=0;
    for(uint32_t level=0;level<std::min(maxLevels,3u);++level)
    {
        const size_t target=std::max<size_t>(96,(original.size()>>(level+1))/3*3);
        if(target>=current.size()) break;
        std::vector<uint32_t> reduced(current.size());
        float error=0;
        const size_t count=meshopt_simplifyWithAttributes(reduced.data(),current.data(),current.size(),
            positions.front().data(),compactCount,sizeof(positions.front()),attributes.front().data(),sizeof(attributes.front()),weights,5,nullptr,
            target,0.04f,meshopt_SimplifyLockBorder,&error);
        if(!count || count%3 || count>=current.size()-current.size()/10 || !std::isfinite(error)) break;
        reduced.resize(count);
        // meshopt includes weighted normal/UV error in this metric. Scaling it
        // to object units supports the existing pixel-selection heuristic; it
        // is not a strict geometric-distance bound.
        cumulativeError+=error*scale;
        if(!std::isfinite(cumulativeError)) break;
        meshopt_optimizeVertexCache(reduced.data(),reduced.data(),count,compactCount);
        MeshLod lod;lod.indices.resize(count);
        lod.error=cumulativeError;
        for(size_t i=0;i<count;++i) lod.indices[i]=representatives[reduced[i]];
        result.push_back(std::move(lod));
        current=std::move(reduced);
    }
    return result;
}

uint32_t Renderer::Impl::EntityLod(uint32_t entity) const
{
    return entity<entityLods.size() ? entityLods[entity] : 0;
}

Math::float4 Renderer::Impl::LodColor(uint32_t level)
{
    constexpr float colors[4][3]={{0.1f,0.8f,0.25f},{0.95f,0.8f,0.08f},{1.0f,0.3f,0.04f},{0.6f,0.15f,1.0f}};
    const auto& c=colors[std::min(level,3u)];
    return {c[0],c[1],c[2],1};
}

void Renderer::Impl::SelectLods(const Scene& scene,const Camera& camera,uint32_t width,uint32_t height,
    const std::vector<RendererDrawDesc>* draws)
{
    const std::weak_ptr<Scene> current=scene.m_lifetime;
    const bool same=!lodScene.expired() && !lodScene.owner_before(current) && !current.owner_before(lodScene);
    if(!same) {entityLods.clear();lodMeshSources.clear();}
    lodScene=current;
    entityLods.resize(scene.GetEntityCount(),0);
    lodMeshSources.resize(scene.GetEntityCount());
    const bool active=config.enableLod && stockDrawInputs && !m_environmentCapture;
    wireframeActive=config.debugWireframe && UsesAdvancedRendering() && stockDrawInputs && !m_environmentCapture;
    lodDebugActive=(config.debugLodColor || wireframeActive) && stockDrawInputs && !m_environmentCapture;
    if(!active)
    {
        std::fill(entityLods.begin(),entityLods.end(),uint8_t{0});
        return;
    }
    const float projection=std::max(std::abs(camera.projection.m[0])*width,std::abs(camera.projection.m[5])*height)*0.5f;
    const bool perspective=std::abs(camera.projection.m[15])<1e-6f;
    const bool gpu=UsesGpuVisibility();
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto material=scene.GetEntityMaterial(entity);
        if(gpu && IsValid(material) && ToIndex(material)<scene.Materials().size() &&
            scene.Materials()[ToIndex(material)].alphaMode!=MaterialAlphaMode::Blend)
        {
            // 불투명 객체의 단계 선택은 GPU가 수행하므로 CPU에서 반복하지 않는다.
            entityLods[i]=0;continue;
        }
        const auto meshId=scene.GetEntityMesh(entity);
        if(!IsValid(meshId) || ToIndex(meshId)>=m_meshes.size()) {entityLods[i]=0;continue;}
        const auto& mesh=m_meshes[ToIndex(meshId)];
        const auto source=mesh.source.lock();
        uint32_t previous=lodMeshSources[i].lock()==source ? entityLods[i] : 0;
        lodMeshSources[i]=source;
        entityLods[i]=0;
        // 변형 뒤의 축약 오차를 아직 저장하지 않은 입력은 원본 단계를 유지한다.
        if(draws && (*draws)[i].vertexBuffer) continue;
        if(mesh.lods.empty() || !mesh.bounds.valid || !std::isfinite(projection)) continue;
        const auto& world=scene.GetTransform(entity);
        // ||A||2 <= sqrt(||A||1 * ||A||infinity), also conservative for shear.
        float columnMax=0,rowMax=0;
        for(uint32_t axis=0;axis<3;++axis)
        {
            float column=0,row=0;
            for(uint32_t k=0;k<3;++k) {column+=std::abs(world.m[axis*4+k]);row+=std::abs(world.m[k*4+axis]);}
            columnMax=std::max(columnMax,column);rowMax=std::max(rowMax,row);
        }
        const float scale=std::sqrt(columnMax*rowMax);
        if(!std::isfinite(scale) || scale<=0) continue;
        const auto center=Math::TransformPoint(world,(mesh.bounds.min+mesh.bounds.max)*0.5f);
        const float radius=std::sqrt(Math::LengthSquared((mesh.bounds.max-mesh.bounds.min)*0.5f))*scale;
        const float depth=-Math::TransformPoint(camera.view,center).z;
        if(perspective && depth<=radius+0.001f) continue;
        const float pixelsPerUnit=projection*(perspective ? 1.0f/(depth-radius) : 1.0f);
        if(!std::isfinite(pixelsPerUnit) || pixelsPerUnit<0) continue;
        previous=std::min<uint32_t>(previous,mesh.lods.size());
        uint32_t selected=0;
        for(uint32_t level=1;level<=mesh.lods.size();++level)
        {
            const auto& lod=mesh.lods[level-1];
            const float limit=config.lodPixelError*(level>previous ? 1-config.lodHysteresis : 1+config.lodHysteresis);
            const float projected=lod.error*scale*pixelsPerUnit;
            if(!lod.ready || !lod.buffer || !std::isfinite(projected) || projected>limit) break;
            selected=level;
        }
        entityLods[i]=static_cast<uint8_t>(selected);
    }
}
}
