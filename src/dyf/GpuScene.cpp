#include "RendererInternal.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "GpuVisibility.h"
#include "SceneAlgorithms.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>

namespace dyf
{
// 스칼라·텍스처 핸들·플래그 해시로 후보를 좁힌 뒤 전체 동등성을 확인해 대표 재질을 공유한다. 해시 충돌만으로 합치지 않는다.
namespace
{
template<class States>
class MaterialCanonicalizer
{
    const Scene& m_scene;
    const States& m_states;
    std::vector<uint32_t> m_representatives;
    struct Candidate {uint32_t material,next;};
    std::vector<Candidate> m_candidates;
    std::unordered_map<size_t,uint32_t> m_buckets;
public:
    MaterialCanonicalizer(const Scene& scene,const States& states)
        :m_scene(scene),m_states(states),m_representatives(scene.Materials().size(),UINT32_MAX)
    {
        const auto capacity=std::min(scene.Materials().size(),size_t(scene.GetEntityCount()));
        m_candidates.reserve(capacity);m_buckets.reserve(capacity);
    }
    uint32_t operator()(uint32_t index)
    {
        if(m_representatives[index]!=UINT32_MAX)return m_representatives[index];
        const auto& material=m_scene.Materials()[index];
        const auto& state=m_states[index];
        size_t hash=MaterialScalarHash(material);
        const auto combine=[&](size_t value) {hash^=value+size_t{0x9e3779b9}+(hash<<6)+(hash>>2);};
        combine(std::hash<uint32_t>{}(state.textureFlags));
        for(const auto texture:state.textures)combine(std::hash<RHI::TextureHandle>{}(texture));
        // Chain colliding representatives in contiguous storage instead of
        // allocating a separate vector for every distinct material hash.
        auto& first=m_buckets.try_emplace(hash,UINT32_MAX).first->second;
        for(auto entry=first;entry!=UINT32_MAX;entry=m_candidates[entry].next)
        {
            const auto candidate=m_candidates[entry].material;
            const auto& representative=m_states[candidate];
            if(state.textureFlags==representative.textureFlags && state.textures==representative.textures &&
                EqualMaterial(material,m_scene.Materials()[candidate]))
                return m_representatives[index]=candidate;
        }
        m_candidates.push_back({index,first});
        first=static_cast<uint32_t>(m_candidates.size()-1);
        return m_representatives[index]=index;
    }
};
}

// 설정 요청뿐 아니라 인스턴싱·표준 셰이더·Compute·IndirectDraw 지원을 함께 검사한다. 환경 캡처는 이 경로에서 제외한다.
bool Renderer::Impl::UsesGpuVisibility() const
{
    return config.enableGpuCulling && config.enableInstancing && UsesStockGeometry() &&
        shaderSources.bytes[MeshFragment].empty() && !m_environmentCapture && device->Supports(RHI::Feature::Compute) &&
        device->Supports(RHI::Feature::IndirectDraw);
}

// 내용이 같은 재질을 대표 재질로 모으고, 메시·재질·정점 버퍼·그림자 수신·면 방향이 같은 객체를 그룹으로 묶는다. 그룹별 LOD 배치와 객체별 경계/행렬 입력을 만든 뒤 각 배치의 출력 구간을 정하고 업로드를 기록한다.
bool Renderer::Impl::PrepareVisibility(const Scene& scene,const Camera& camera,RHI::ICommandList& uploads,
    uint32_t width,uint32_t height,const std::vector<RendererDrawDesc>* draws)
{
    visibilityActive=false;visibilityDraws.clear();
    if(!UsesGpuVisibility() || !stockDrawInputs) return true; // 장치/설정/입력 계약이 맞지 않으면 GPU 경로만 생략하고 CPU 렌더는 계속할 수 있다.
    if(!PrepareMainIndirectGeometry(scene,device))return false;
    // 같은 메시·동등 재질·현재/이전 정점 버퍼·그림자 수신·반전 여부인 객체만 같은 그룹으로 묶는다. 재질 ID가 달라도 실제 내용이 같으면 대표 재질로 통합할 수 있다.
    using Key=std::tuple<uint32_t,uint32_t,uint32_t,uintptr_t,uintptr_t,bool,bool,bool>;
    std::map<Key,std::array<uint32_t,4>> groups;
    std::vector<GpuVisibility::Batch> batches;
    std::vector<GpuVisibility::Instance> inputs;
    std::vector<uint64_t> keys;
    inputs.reserve(scene.GetEntityCount());keys.reserve(scene.GetEntityCount());
    MaterialCanonicalizer canonicalMaterial(scene,materialStates);
    // 객체 ID 순서로 후보를 수집하고 Blend·투과 표면은 Forward 정렬 또는 별도 투명 패스에 남긴다.
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto meshId=scene.GetEntityMesh(entity);
        const auto materialId=scene.GetEntityMaterial(entity);
        if(!IsValid(meshId) || !IsValid(materialId)) continue; // 유효한 메시/재질 ID가 없는 객체는 후보에서 제외한다.
        const auto meshIndex=ToIndex(meshId),sourceMaterialIndex=ToIndex(materialId);
        if(meshIndex>=m_meshes.size() || sourceMaterialIndex>=scene.Materials().size() || // 배열 범위를 벗어난 메시/재질 참조는 접근하지 않는다.
            sourceMaterialIndex>=materialStates.size()) continue;
        const uint32_t materialIndex=canonicalMaterial(sourceMaterialIndex);
        const auto& mesh=m_meshes[meshIndex];
        if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount || // GPU 메시가 준비되지 않았거나 Blend 재질이면 이 불투명 간접 경로에서 제외한다.
            UsesBlendedRendering(scene.Materials()[materialIndex])) continue;
        const auto* input=draws ? &(*draws)[i] : nullptr;
        const auto vertex=input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer;
        const auto previous=input && input->previousVertexBuffer ? input->previousVertexBuffer : vertex;
        // 외부 변형 정점 버퍼에는 원본 LOD의 적합성을 가정하지 않고 기본 단계만 쓴다. 원본 메시 경로만 준비된 LOD를 최대 네 단계까지 펼친다.
        uint32_t levels=1;
        if(!input || !input->vertexBuffer) // 외부 정점 교체가 없는 원본 메시만 준비된 LOD 목록을 사용한다.
            for(const auto& lod:mesh.lods)
            {
                if(!lod.ready || !lod.buffer || levels==4) break; // 준비되지 않은 LOD 또는 네 단계 상한에 도달하면 확장을 멈춘다.
                ++levels;
            }
        // Explicit source-buffer overrides still disable source LODs. Matching
        // effective buffer pointers must not reuse a group's shorter batch array.
        const Key key{meshIndex,materialIndex,levels,reinterpret_cast<uintptr_t>(vertex),
            reinterpret_cast<uintptr_t>(previous),scene.GetEntityLighting(entity).receiveShadow,
            Math::Determinant3x3(scene.GetTransform(entity))<0,!input || input->deformationHistoryValid};
        auto found=groups.find(key);
        if(found==groups.end()) // 새 호환성 그룹을 처음 만나면 LOD별 간접 배치와 대표 draw를 만든다.
        {
            std::array<uint32_t,4> ids={};
            for(uint32_t level=0;level<levels;++level)
            {
                if(batches.size()>=UINT32_MAX) return false; // 32비트 배치 ID로 표현할 수 없으면 실패한다.
                ids[level]=static_cast<uint32_t>(batches.size());
                GpuVisibility::Batch batch;
                batch.indexCount=level ? mesh.lods[level-1].count : mesh.indexCount;
                const auto indices=MainIndirectIndexBuffer(mesh,level,scene.Materials()[materialIndex],input);
                if(indices==mesh.indirectIndexBuffer && mesh.indirectIndexReady)
                {
                    batch.firstIndex=mesh.indirectFirstIndices[level];
                    assert(uint64_t(batch.firstIndex)+batch.indexCount<=indices->GetDesc().size/sizeof(uint32_t));
                }
                batches.push_back(batch);visibilityDraws.push_back({i,meshIndex,materialIndex,level});
            }
            found=groups.emplace(key,ids).first;
        }
        // 현재 모델과 이전 MVP를 넣는다. 유효한 장면 이력이 없으면 현재 MVP로 시작하고 변형 이력이 끊겼으면 previousMVP를 0으로 표시한다.
        GpuVisibility::Instance value;
        value.model=scene.GetTransform(entity);value.lodCount=levels;value.batches=found->second;
        value.previousMVP=frameHistoryValid && i<previousModels.size()
            ? previousViewProjection*previousModels[i] : camera.projection*camera.view*value.model;
        if(input && !input->deformationHistoryValid) value.previousMVP={}; // 변형의 이전 상태가 유효하지 않으면 이전 MVP 사용을 막는다.
        const auto& bounds=input && input->hasLocalBounds ? input->localBounds : mesh.bounds;
        // 월드 변환의 절대 행/열 합으로 보수적인 스케일 상한을 구한다. 전단·비균일 스케일에서도 구의 반경을 작게 잡아 객체를 잘못 제거하지 않으려는 계산이다.
        float columnMax=0,rowMax=0;
        for(uint32_t axis=0;axis<3;++axis)
        {
            float column=0,row=0;
            for(uint32_t k=0;k<3;++k)
            {column+=std::abs(value.model.m[axis*4+k]);row+=std::abs(value.model.m[k*4+axis]);}
            columnMax=std::max(columnMax,column);rowMax=std::max(rowMax,row);
        }
        const float scale=std::sqrt(columnMax*rowMax);
        // 유효한 경계·유한 스케일·affine 변환에만 월드 구를 만들어 보수적인 가시성 판정을 허용한다.
        // A sphere in world xyz cannot bound a varying homogeneous w. Unknown
        // projective transforms stay visible and use level zero in the shader.
        if(bounds.valid && std::isfinite(scale) && IsAffineTransform(value.model))
        {
            const auto center=Math::TransformPoint(value.model,(bounds.min+bounds.max)*.5f);
            const float radius=Math::Length((bounds.max-bounds.min)*.5f)*scale;
            value.centerRadius={center.x,center.y,center.z,radius};
            value.flags=std::isfinite(radius) && std::isfinite(center.x) &&
                std::isfinite(center.y) && std::isfinite(center.z) ? 1u : 0u;
        }
        else value.flags=0; // 경계를 신뢰할 수 없으면 유효 경계 플래그를 끈다. 임의의 작은 구로 대체하지 않는다.
        // LOD 오차를 월드 스케일에 맞추고 각 배치가 담을 수 있어야 할 후보 수를 늘린다. 실제 가시 수는 이후 GPU가 정한다.
        float* errors=&value.lodErrors.x;
        for(uint32_t level=0;level<levels;++level)
        {
            errors[level]=level ? mesh.lods[level-1].error*scale : 0;
            auto& capacity=batches[value.batches[level]].capacity;
            if(capacity==UINT32_MAX) return false; // 배치별 후보 개수 증가가 32비트 범위를 넘지 않도록 막는다.
            ++capacity;
        }
        inputs.push_back(value);
        // GPU 입력 순서별로 원본 객체 ID와 메시 인덱스를 저장한다. 이 키 배열이 직전 프레임과 같을 때만 같은 객체의 LOD 이력으로 취급한다.
        keys.push_back((uint64_t(i)<<32)|meshIndex);
    }
    if(inputs.empty()) return true; // 후보가 없으면 GPU 자원 준비 없이 성공한다.
    // 배치별 최악의 가시 인스턴스 구간을 하나의 출력 버퍼 안에 연속 배치한다. 누적 계산은 64비트로 해 오버플로를 먼저 검사한다.
    uint64_t offset=0;
    for(auto& batch:batches)
    {
        batch.instanceOffset=static_cast<uint32_t>(offset);offset+=batch.capacity;
        if(offset>UINT32_MAX/sizeof(GpuVisibility::VisibleInstance)) return false; // 가시 인스턴스 버퍼 바이트 용량이 32비트를 넘으면 실패한다.
    }
    if(!gpuVisibility) gpuVisibility=std::make_unique<GpuVisibility>(*device); // GPU 가시성 관리자가 없을 때 같은 장치로 지연 생성한다.
    // 카메라·화면 크기·LOD 오차/여유 폭·CPU 설정을 GPU 계산 입력으로 전달한다. 행 순서나 Scene 소유자가 달라지면 이전 GPU LOD 이력을 끊는다.
    GpuVisibility::Settings settings;
    settings.camera=camera;settings.width=width;settings.height=height;
    settings.pixelError=config.lodPixelError;settings.hysteresis=config.lodHysteresis;
    settings.screenSize=config.lodScreenSize;
    settings.frustum=config.enableFrustumCulling;settings.lod=config.enableLod;
    const std::weak_ptr<Scene> owner=scene.m_lifetime;
    settings.resetHistory=keys!=visibilityKeys || visibilityScene.expired() ||
        visibilityScene.owner_before(owner) || owner.owner_before(visibilityScene);
    if(!gpuVisibility->Prepare(inputs,batches,settings) || !gpuVisibility->RecordUploads(uploads)) return false; // 자원 준비 또는 업로드 기록 실패면 visibilityActive를 켜지 않는다.
    // 준비와 업로드 기록이 성공한 뒤에만 이번 경로를 active로 표시한다. 이 시점은 아직 GPU compute나 draw 완료 시점이 아니다.
    visibilityKeys=std::move(keys);visibilityScene=owner;visibilityActive=true;return true;
}

bool Renderer::Impl::PrepareCpuBatches(const Scene& scene,const Camera& camera,RHI::ICommandList& uploads,
    const std::vector<RendererDrawDesc>* draws,bool motion)
{
    // Custom fragment pipelines may blend even nominally opaque materials.
    // Keep their submission order on the legacy path rather than globally sorting.
    cpuBatchesActive=!visibilityActive && stockDrawInputs && UsesInstanceStream() &&
        shaderSources.bytes[MeshFragment].empty();
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
            ToIndex(material)<scene.Materials().size() && !UsesBlendedRendering(scene.Materials()[ToIndex(material)]) &&
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
            UsesBlendedRendering(scene.Materials()[materialIndex]))continue;
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
