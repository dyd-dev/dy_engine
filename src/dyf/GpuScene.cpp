#include "RendererInternal.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "GpuVisibility.h"
#include "SceneAlgorithms.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>

namespace dyf
{
namespace
{
template<class States>
class MaterialCanonicalizer
{
    const Scene& m_scene;
    const States& m_states;
    std::vector<uint32_t> m_representatives;
    std::unordered_map<size_t,std::vector<uint32_t>> m_buckets;
public:
    MaterialCanonicalizer(const Scene& scene,const States& states)
        :m_scene(scene),m_states(states),m_representatives(scene.Materials().size(),UINT32_MAX)
    {m_buckets.reserve(std::min(scene.Materials().size(),size_t(scene.GetEntityCount())));}
    uint32_t operator()(uint32_t index)
    {
        if(m_representatives[index]!=UINT32_MAX)return m_representatives[index];
        const auto& material=m_scene.Materials()[index];
        const auto& state=m_states[index];
        size_t hash=MaterialScalarHash(material);
        const auto combine=[&](size_t value) {hash^=value+size_t{0x9e3779b9}+(hash<<6)+(hash>>2);};
        combine(std::hash<uint32_t>{}(state.textureFlags));
        for(const auto texture:state.textures)combine(std::hash<RHI::TextureHandle>{}(texture));
        auto& candidates=m_buckets[hash];
        for(const auto candidate:candidates)
        {
            const auto& representative=m_states[candidate];
            if(state.textureFlags==representative.textureFlags && state.textures==representative.textures &&
                EqualMaterial(material,m_scene.Materials()[candidate]))
                return m_representatives[index]=candidate;
        }
        candidates.push_back(index);
        return m_representatives[index]=index;
    }
};
}

bool Renderer::Impl::UsesGpuVisibility() const
{
    return config.enableGpuCulling && config.enableInstancing && UsesStockGeometry() &&
        !m_environmentCapture && device->Supports(RHI::Feature::Compute) &&
        device->Supports(RHI::Feature::IndirectDraw);
}

bool Renderer::Impl::PrepareVisibility(const Scene& scene,const Camera& camera,RHI::ICommandList& uploads,
    uint32_t width,uint32_t height,const std::vector<RendererDrawDesc>* draws)
{
    visibilityActive=false;visibilityDraws.clear();
    if(!UsesGpuVisibility() || !stockDrawInputs) return true;
    using Key=std::tuple<uint32_t,uint32_t,uintptr_t,uintptr_t,bool,bool>;
    std::map<Key,std::array<uint32_t,4>> groups;
    std::vector<GpuVisibility::Batch> batches;
    std::vector<GpuVisibility::Instance> inputs;
    std::vector<uint64_t> keys;
    inputs.reserve(scene.GetEntityCount());keys.reserve(scene.GetEntityCount());
    MaterialCanonicalizer canonicalMaterial(scene,materialStates);
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto meshId=scene.GetEntityMesh(entity);
        const auto materialId=scene.GetEntityMaterial(entity);
        if(!IsValid(meshId) || !IsValid(materialId)) continue;
        const auto meshIndex=ToIndex(meshId),sourceMaterialIndex=ToIndex(materialId);
        if(meshIndex>=m_meshes.size() || sourceMaterialIndex>=scene.Materials().size() ||
            sourceMaterialIndex>=materialStates.size()) continue;
        const uint32_t materialIndex=canonicalMaterial(sourceMaterialIndex);
        const auto& mesh=m_meshes[meshIndex];
        if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount ||
            scene.Materials()[materialIndex].alphaMode==MaterialAlphaMode::Blend) continue;
        const auto* input=draws ? &(*draws)[i] : nullptr;
        const auto vertex=input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer;
        const auto previous=input && input->previousVertexBuffer ? input->previousVertexBuffer : vertex;
        const Key key{meshIndex,materialIndex,reinterpret_cast<uintptr_t>(vertex),
            reinterpret_cast<uintptr_t>(previous),scene.GetEntityLighting(entity).receiveShadow,
            Math::Determinant3x3(scene.GetTransform(entity))<0};
        uint32_t levels=1;
        if(!input || !input->vertexBuffer)
            for(const auto& lod:mesh.lods)
            {
                if(!lod.ready || !lod.buffer || levels==4) break;
                ++levels;
            }
        auto found=groups.find(key);
        if(found==groups.end())
        {
            std::array<uint32_t,4> ids={};
            for(uint32_t level=0;level<levels;++level)
            {
                if(batches.size()>=UINT32_MAX) return false;
                ids[level]=static_cast<uint32_t>(batches.size());
                GpuVisibility::Batch batch;
                batch.indexCount=level ? mesh.lods[level-1].count : mesh.indexCount;
                batches.push_back(batch);visibilityDraws.push_back({i,meshIndex,materialIndex,level});
            }
            found=groups.emplace(key,ids).first;
        }
        GpuVisibility::Instance value;
        value.model=scene.GetTransform(entity);value.lodCount=levels;value.batches=found->second;
        value.previousMVP=frameHistoryValid && i<previousModels.size()
            ? previousViewProjection*previousModels[i] : camera.projection*camera.view*value.model;
        if(input && !input->deformationHistoryValid) value.previousMVP={};
        const auto& bounds=input && input->hasLocalBounds ? input->localBounds : mesh.bounds;
        float columnMax=0,rowMax=0;
        for(uint32_t axis=0;axis<3;++axis)
        {
            float column=0,row=0;
            for(uint32_t k=0;k<3;++k)
            {column+=std::abs(value.model.m[axis*4+k]);row+=std::abs(value.model.m[k*4+axis]);}
            columnMax=std::max(columnMax,column);rowMax=std::max(rowMax,row);
        }
        const float scale=std::sqrt(columnMax*rowMax);
        if(bounds.valid && std::isfinite(scale))
        {
            const auto center=Math::TransformPoint(value.model,(bounds.min+bounds.max)*.5f);
            const float radius=Math::Length((bounds.max-bounds.min)*.5f)*scale;
            value.centerRadius={center.x,center.y,center.z,radius};
            value.flags=std::isfinite(radius) && std::isfinite(center.x) &&
                std::isfinite(center.y) && std::isfinite(center.z) ? 1u : 0u;
        }
        else value.flags=0;
        float* errors=&value.lodErrors.x;
        for(uint32_t level=0;level<levels;++level)
        {
            errors[level]=level ? mesh.lods[level-1].error*scale : 0;
            auto& capacity=batches[value.batches[level]].capacity;
            if(capacity==UINT32_MAX) return false;
            ++capacity;
        }
        inputs.push_back(value);
        // 입력 행 번호와 geometry가 같을 때만 GPU LOD 이력을 이어 쓴다.
        keys.push_back((uint64_t(i)<<32)|meshIndex);
    }
    if(inputs.empty()) return true;
    uint64_t offset=0;
    for(auto& batch:batches)
    {
        batch.instanceOffset=static_cast<uint32_t>(offset);offset+=batch.capacity;
        if(offset>UINT32_MAX/sizeof(GpuVisibility::VisibleInstance)) return false;
    }
    if(!gpuVisibility) gpuVisibility=std::make_unique<GpuVisibility>(*device);
    GpuVisibility::Settings settings;
    settings.camera=camera;settings.width=width;settings.height=height;
    settings.pixelError=config.lodPixelError;settings.hysteresis=config.lodHysteresis;
    settings.frustum=config.enableFrustumCulling;settings.lod=config.enableLod;
    const std::weak_ptr<Scene> owner=scene.m_lifetime;
    settings.resetHistory=keys!=visibilityKeys || visibilityScene.expired() ||
        visibilityScene.owner_before(owner) || owner.owner_before(visibilityScene);
    if(!gpuVisibility->Prepare(inputs,batches,settings) || !gpuVisibility->RecordUploads(uploads)) return false;
    visibilityKeys=std::move(keys);visibilityScene=owner;visibilityActive=true;return true;
}

bool Renderer::Impl::PrepareCpuBatches(const Scene& scene,const Camera& camera,RHI::ICommandList& uploads,
    const std::vector<RendererDrawDesc>* draws,bool motion)
{
    cpuBatchesActive=!visibilityActive && stockDrawInputs && UsesInstanceStream();
    cpuDrawInputs.clear();cpuDraws.clear();cpuInstanceScratch.clear();cpuInstanceEntities.clear();cpuCulledEntities=0;
    cpuBatchIndices.clear();
    if(!cpuBatchesActive)return true;
    // Unique meshes cannot instance. Keep their existing draw path without sorting or repacking.
    cpuMeshCounts.assign(m_meshes.size(),0);
    bool repeated=false;
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto mesh=scene.GetEntityMesh(entity);
        const auto material=scene.GetEntityMaterial(entity);
        if(IsValid(mesh) && ToIndex(mesh)<cpuMeshCounts.size() && IsValid(material) &&
            ToIndex(material)<scene.Materials().size() && scene.Materials()[ToIndex(material)].alphaMode!=MaterialAlphaMode::Blend &&
            ++cpuMeshCounts[ToIndex(mesh)]>1) {repeated=true;break;}
    }
    if(!repeated) {cpuBatchesActive=false;return true;}
    cpuBatchIndices.assign(scene.GetEntityCount(),UINT32_MAX);
    cpuDrawInputs.reserve(scene.GetEntityCount());
    MaterialCanonicalizer canonicalMaterial(scene,materialStates);
    const CameraFrustum frustum(camera);
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto meshId=scene.GetEntityMesh(entity);
        const auto materialId=scene.GetEntityMaterial(entity);
        if(!IsValid(meshId) || !IsValid(materialId))continue;
        const auto meshIndex=ToIndex(meshId),materialIndex=ToIndex(materialId);
        if(meshIndex>=m_meshes.size() || materialIndex>=materialStates.size() ||
            scene.Materials()[materialIndex].alphaMode==MaterialAlphaMode::Blend)continue;
        const auto& mesh=m_meshes[meshIndex];
        if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount)continue;
        const auto* input=draws ? &(*draws)[i] : nullptr;
        const auto& bounds=input && input->hasLocalBounds ? input->localBounds : mesh.bounds;
        const auto& world=scene.GetTransform(entity);
        if(config.enableFrustumCulling && frustum.Outside(bounds,world)) {++cpuCulledEntities;continue;}
        const auto vertex=input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer;
        const auto previous=input && input->previousVertexBuffer ? input->previousVertexBuffer : vertex;
        const auto level=std::min<uint32_t>(EntityLod(i),mesh.lods.size());
        CpuDrawKey key{meshIndex,canonicalMaterial(materialIndex),level,reinterpret_cast<uintptr_t>(vertex),
            reinterpret_cast<uintptr_t>(previous),scene.GetEntityLighting(entity).receiveShadow,
            Math::Determinant3x3(world)<0,!input || input->deformationHistoryValid};
        cpuDrawInputs.push_back({key,i});
    }
    std::sort(cpuDrawInputs.begin(),cpuDrawInputs.end(),[](const CpuDrawInput& a,const CpuDrawInput& b) {
        return a.key!=b.key ? a.key<b.key : a.entity<b.entity;
    });
    constexpr uint32_t stride=2*sizeof(Math::float4x4);
    if(cpuDrawInputs.size()>UINT32_MAX/stride)return false;
    cpuInstanceScratch.reserve(cpuDrawInputs.size()*2);cpuInstanceEntities.reserve(cpuDrawInputs.size());
    const auto viewProjection=motion ? camera.projection*camera.view : Math::float4x4{};
    for(size_t i=0;i<cpuDrawInputs.size();++i)
    {
        const auto& input=cpuDrawInputs[i];
        if(!i || input.key!=cpuDrawInputs[i-1].key)
        {
            cpuBatchIndices[input.entity]=static_cast<uint32_t>(cpuDraws.size());
            cpuDraws.push_back({input.entity,std::get<0>(input.key),std::get<1>(input.key),std::get<2>(input.key),
                static_cast<uint32_t>(i),0});
        }
        ++cpuDraws.back().instanceCount;
        const auto& world=scene.GetTransform(static_cast<EntityID>(input.entity));
        cpuInstanceScratch.push_back(world);
        Math::float4x4 previous={};
        if(motion)previous=frameHistoryValid && input.entity<previousModels.size()
            ? previousViewProjection*previousModels[input.entity] : viewProjection*world;
        if(draws && !(*draws)[input.entity].deformationHistoryValid)previous={};
        cpuInstanceScratch.push_back(previous);cpuInstanceEntities.push_back(input.entity);
    }
    if(cpuInstanceScratch.empty())return true;
    const auto bytes=static_cast<uint32_t>(cpuInstanceScratch.size()*sizeof(Math::float4x4));
    if(!cpuInstanceBuffer || cpuInstanceBuffer->GetDesc().size<bytes)
    {
        const uint64_t capacity=std::min<uint64_t>(UINT32_MAX/stride*stride,
            std::max<uint64_t>(bytes,cpuInstanceBuffer ? uint64_t(cpuInstanceBuffer->GetDesc().size)*2 : stride*64));
        cpuInstanceBuffer=device->CreateBuffer({static_cast<uint32_t>(capacity),stride,
            RHI::BufferUsage::Vertex,RHI::ResourceState::CopyDestination});
        cpuInstanceReady=false;
    }
    if(!cpuInstanceBuffer)return false;
    if(config.enableIncrementalTransformUploads && cpuInstanceReady && cpuInstanceBytes.size()==cpuInstanceScratch.size() &&
        std::memcmp(cpuInstanceBytes.data(),cpuInstanceScratch.data(),bytes)==0)return true;
    if(cpuInstanceReady)
    {
        const RHI::ResourceBarrierDesc before={cpuInstanceBuffer,nullptr,RHI::ResourceState::VertexBuffer,
            RHI::ResourceState::CopyDestination,{}};
        uploads.ResourceBarrier(&before,1);
    }
    if(!device->UpdateBuffer(uploads,cpuInstanceBuffer,0,cpuInstanceScratch.data(),bytes))return false;
    const RHI::ResourceBarrierDesc after={cpuInstanceBuffer,nullptr,RHI::ResourceState::CopyDestination,
        RHI::ResourceState::VertexBuffer,{}};
    uploads.ResourceBarrier(&after,1);
    return true;
}
}
