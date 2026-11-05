// CPU-only work/lifetime regressions; native shader output is covered by RenderingConformanceTests.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#define private public
#include <dyf/Extends/Model/ModelScene.h>
#undef private
#include <dyf/Extends/Model/ModelRenderer.h>
#define main CanvasReuseEntryUnused
#include "../../../../tests/CanvasReuseTests.cpp"
#undef main

namespace dyf
{
struct ModelRuntimeTestAccess
{
    static bool Prepare(ModelRenderer& model,const ModelScene& scene) {return model.Prepare(scene);}
    static const RendererDrawDesc& Draw(const ModelRenderer& model) {return model.m_draws[0];}
    static void Shader(ModelRenderer& model) {model.m_shaderBytes[2]={1};model.m_shaderEntries[2]="main";}
    static void CommitHistory(ModelRenderer& model,const ModelScene& scene)
    {
        model.m_historyScene=&scene;model.m_historyFrame=model.m_frameIndex;
        model.m_historyDraws=model.m_draws;model.m_historyMeshes={scene.Meshes()[0]};
        model.m_historyVertexCounts={scene.Meshes()[0]->vertices.size()};
    }
};
}

namespace { bool countAllocations=false; size_t allocations=0; }
void* operator new(std::size_t size)
{
    if(countAllocations) ++allocations;
    if(void* memory=std::malloc(size?size:1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory,std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory,std::size_t) noexcept { std::free(memory); }

namespace
{
using namespace dyf;

void AnimationReuse()
{
    ModelScene scene;
    ModelAsset asset;
    asset.nodes.resize(128);
    for(size_t index=1;index<asset.nodes.size();++index) asset.nodes[index].parentIndex=int32_t(index-1);
    AnimationClip clip; clip.duration=1;
    NodeAnimationTrack track;track.nodeIndex=0;
    track.translations={{0,{}, {2,0,0},{}},{1,{}, {4,0,0},{}}};
    clip.tracks.push_back(track);asset.animations.push_back(clip);
    const auto assetId=scene.CreateModelAsset(std::move(asset));
    ModelInstance instance;instance.assetId=assetId;
    const auto id=scene.CreateModelInstance(std::move(instance));
    Check(IsValid(id) && bool(scene.Add(CreateCubeMesh())),"immutable pose fixture");
    Check(scene.BindEntityToModel(id,static_cast<EntityID>(0),127),"immutable binding");
    Check(scene.PlayAnimation(id,0) && scene.SetAnimationSpeed(id,0) && scene.UpdateAnimations(0),"frozen playing pose");
    allocations=0;countAllocations=true;
    const bool updated=scene.UpdateAnimations(1);
    countAllocations=false;
    Check(updated && allocations==0,"unchanged immutable pose must reuse owner-local work arrays without allocating");
    Check(scene.GetTransform(static_cast<EntityID>(0)).m[12]==2,"frozen pose transform");
    Check(scene.GetEntity(static_cast<EntityID>(0)).SetPosition({99,0,0}) && scene.UpdateAnimations(0),"external entity transform update");
    Check(scene.GetTransform(static_cast<EntityID>(0)).m[12]==2,"pose reuse must restore externally edited entity transforms");
    Check(scene.SetAnimationSpeed(id,1),"resume moving playback");
    allocations=0;countAllocations=true;
    const bool advanced=scene.UpdateAnimations(.25f);
    countAllocations=false;
    Check(advanced && allocations==0 && scene.GetTransform(static_cast<EntityID>(0)).m[12]==2.5f,
        "advancing immutable poses must retain validation and hierarchy scratch");
    Check(scene.PlayAnimation(id,0) && scene.SetAnimationSpeed(id,0) && scene.UpdateAnimations(0),"restore frozen sample");

    // Holding this reference across updates must keep subsequent direct edits visible.
    auto& mutableInstance=scene.GetModelInstance(id);
    mutableInstance.rootTransform=Math::Translation({3,0,0});
    Check(scene.UpdateAnimations(0) && scene.GetTransform(static_cast<EntityID>(0)).m[12]==5,"direct root edit");
    mutableInstance.bindings[0].nodeIndex=999;
    Check(!scene.UpdateAnimations(0),"direct binding edit must still be validated");
    mutableInstance.bindings[0].nodeIndex=0;
    Check(scene.UpdateAnimations(0),"invalid binding repair must retry");
    mutableInstance.assetId=ModelAssetID::Invalid;
    mutableInstance.nodes.resize(1);mutableInstance.clips={clip};
    Check(scene.UpdateAnimations(0),"mutable legacy animation fixture");
    mutableInstance.clips[0].tracks[0].translations[0].value.x=7;
    Check(scene.UpdateAnimations(0) && scene.GetTransform(static_cast<EntityID>(0)).m[12]==10,"direct clip edit must not be hidden by frozen playback");
    mutableInstance.clips[0].tracks[0].translations[1].time=-1;
    Check(!scene.UpdateAnimations(0),"direct invalid key edit must be rejected");
    mutableInstance.clips[0].tracks[0].translations[1].time=1;
    Check(scene.UpdateAnimations(0),"invalid clip repair must retry");
    mutableInstance.nodes[0].parentIndex=0;
    Check(!scene.UpdateAnimations(0),"direct cyclic hierarchy edit must be rejected");
    mutableInstance.nodes[0].parentIndex=-1;
    Check(scene.UpdateAnimations(0),"invalid hierarchy repair must retry");
}

void MorphPoseReuse()
{
    ModelScene scene;
    Check(bool(scene.Add(CreateCubeMesh())),"morph fixture mesh");
    ModelAsset asset;asset.nodes.resize(1);asset.nodes[0].morphWeights={.5f};
    ModelAssetMesh mesh;mesh.mesh=static_cast<MeshID>(0);mesh.nodeIndex=0;
    MorphTarget target;target.positionDeltas.assign(scene.Meshes()[0]->vertices.size(),{2,0,0});
    mesh.morphTargets.push_back(std::move(target));asset.meshes.push_back(std::move(mesh));
    ModelInstance instance;instance.assetId=scene.CreateModelAsset(std::move(asset));
    const auto id=scene.CreateModelInstance(std::move(instance));
    const auto entity=static_cast<EntityID>(0);
    Check(scene.BindEntityToModel(id,entity,0,UINT32_MAX,0) && scene.UpdateAnimations(0),"morph pose bind");
    const auto baseX=scene.Meshes()[0]->vertices[0].position.x;
    Check(scene.TryGetEntityMorphedMesh(entity)->vertices[0].position.x==baseX+1,"default morph weight");
    allocations=0;countAllocations=true;
    const bool repeated=scene.UpdateAnimations(0);
    countAllocations=false;
    Check(repeated && allocations==0 && scene.TryGetEntityMorphedMesh(entity)->vertices[0].position.x==baseX+1,
        "unchanged morph output must survive a frozen update");
    Check(scene.SetMorphWeight(id,0,0,1) && scene.UpdateAnimations(0) &&
        scene.TryGetEntityMorphedMesh(entity)->vertices[0].position.x==baseX+2,"morph override invalidates frozen pose");
    Check(scene.ClearMorphWeightOverride(id,0,0) && scene.UpdateAnimations(0) &&
        scene.TryGetEntityMorphedMesh(entity)->vertices[0].position.x==baseX+1,"clearing morph override restores sampled weights");
    Check(scene.SetMorphWeight(id,0,0,0) && scene.UpdateAnimations(0) && !scene.TryGetEntityMorphedMesh(entity) &&
        scene.HasEntityMorphTargets(entity),"zero morph weight clears current vertices while retaining morph history eligibility");
}

void RegistrationSnapshots()
{
    ModelScene scene;
    ModelAsset asset;asset.nodes.resize(1);asset.animations.resize(1);
    asset.animations[0].tracks.resize(1);
    asset.animations[0].tracks[0].translations.resize(1);
    const auto nodeAddress=reinterpret_cast<uintptr_t>(asset.nodes.data());
    const auto keyAddress=reinterpret_cast<uintptr_t>(asset.animations[0].tracks[0].translations.data());
    const auto assetId=scene.CreateModelAsset(std::move(asset));
    const auto& registered=scene.GetModelAsset(assetId);
    const bool assetDetached=reinterpret_cast<uintptr_t>(registered.nodes.data())!=nodeAddress &&
        reinterpret_cast<uintptr_t>(registered.animations[0].tracks[0].translations.data())!=keyAddress;
    ModelInstance instance;instance.assetId=assetId;instance.bindings.resize(1);
    const auto bindingAddress=reinterpret_cast<uintptr_t>(instance.bindings.data());
    const auto id=scene.CreateModelInstance(std::move(instance));
    Check(IsValid(id),"snapshot instance registration");
    const auto& stored=static_cast<const ModelScene&>(scene).GetModelInstance(id);
    // Compare captured addresses without dereferencing aliases whose lifetime may have ended.
    Check(assetDetached && reinterpret_cast<uintptr_t>(stored.bindings.data())!=bindingAddress,
        "owned registration snapshots must detach caller-held writable asset and binding aliases");
}

void PlaybackControls()
{
    ModelScene scene;Check(bool(scene.Add(CreateCubeMesh())),"playback controls mesh");
    ModelAsset asset;asset.nodes.resize(1);
    AnimationClip clip;clip.duration=1;
    NodeAnimationTrack track;track.nodeIndex=0;
    track.translations={{0,{}, {2,0,0},{}},{1,{}, {4,0,0},{}}};
    clip.tracks.push_back(track);asset.animations.push_back(clip);
    clip.duration=0;asset.animations.push_back(clip);
    ModelInstance instance;instance.assetId=scene.CreateModelAsset(std::move(asset));
    const auto id=scene.CreateModelInstance(std::move(instance));
    Check(scene.BindEntityToModel(id,static_cast<EntityID>(0),0) && scene.PlayAnimation(id,0,false) &&
        scene.UpdateAnimations(1),"non-loop endpoint");
    AnimationPlayback playback;
    Check(scene.GetAnimationPlayback(id,playback) && playback.time==1 && !playback.playing,"clip ends at its duration");
    Check(scene.SetAnimationLoop(id,true) && scene.SetAnimationPaused(id,false) && scene.UpdateAnimations(0),"loop endpoint resume");
    Check(scene.GetAnimationPlayback(id,playback) && playback.time==0 &&
        scene.GetTransform(static_cast<EntityID>(0)).m[12]==2,"control changes must normalize loop time even at zero delta");
    Check(scene.PlayAnimation(id,1) && scene.UpdateAnimations(0) && scene.SetAnimationPaused(id,false) &&
        scene.UpdateAnimations(0),"zero-duration resume");
    Check(scene.GetAnimationPlayback(id,playback) && !playback.playing,"zero-duration clips must stop again after resume");
}

struct ComputePipeline final:RHI::Pipeline
{ explicit ComputePipeline(const RHI::PipelineLayoutDesc& layout):Pipeline(layout,true){} };
struct ComputeCommands final:Commands
{
    unsigned& dispatches;
    explicit ComputeCommands(unsigned& count):dispatches(count){}
    void BindComputePipelineNative(RHI::PipelineHandle) override {}
    void DispatchNative(uint32_t,uint32_t,uint32_t) override {++dispatches;}
};
struct ComputeDevice final:Device
{
    unsigned dispatches=0;
    std::unordered_set<RHI::PipelineHandle> computePipelines;
    bool SupportsNative(RHI::Feature feature) const override
    { return feature==RHI::Feature::Compute || Device::SupportsNative(feature); }
    RHI::ICommandList* AcquireCommandListNative() override {return new ComputeCommands(dispatches);}
    RHI::PipelineHandle CreateComputePipelineNative(const RHI::ComputePipelineDesc& desc) override
    {auto* pipeline=new ComputePipeline(desc.layout);computePipelines.insert(pipeline);return pipeline;}
    void DestroyPipelineNative(RHI::PipelineHandle pipeline) override
    {if(computePipelines.erase(pipeline))delete static_cast<ComputePipeline*>(pipeline);else Device::DestroyPipelineNative(pipeline);}
};

void DeformationReuse()
{
    ComputeDevice device;
    RendererConfig config;config.enableProfilerHud=false;config.enableHdrRendering=true;config.advanced.enabled=true;
    auto renderer=Renderer::Create(device,reinterpret_cast<void*>(1),config);
    Check(bool(renderer),"model renderer fixture");
    ModelScene scene;
    Check(bool(scene.Add(CreateCubeMesh())),"skinned mesh");
    scene.m_meshSkinInfluences.resize(1);
    scene.m_meshSkinInfluences[0].resize(scene.Meshes()[0]->vertices.size());
    for(auto& influence:scene.m_meshSkinInfluences[0])influence.weights.x=1;
    scene.m_jointPaletteMatrices.resize(1);
    scene.m_entitySkinPaletteOffsets={0};
    scene.m_entityMorphedMeshes.resize(1);
    ModelRenderer model(*renderer);
    ModelRuntimeTestAccess::Shader(model);
    auto prepare=[&] {
        Check(ModelRuntimeTestAccess::Prepare(model,scene),"model preparation");
        return ModelRuntimeTestAccess::Draw(model);
    };
    const auto initial=prepare();
    const auto initialDispatches=device.dispatches;
    Check(initialDispatches==1 && initial.localBounds.valid,"initial compute and exact bounds");
    const auto repeated=prepare();
    Check(device.dispatches==initialDispatches,"unchanged deformation must not dispatch compute again");
    Check(repeated.vertexBuffer==initial.vertexBuffer && repeated.geometryRevision==initial.geometryRevision &&
        initial.localBounds.min.x==repeated.localBounds.min.x && initial.localBounds.min.y==repeated.localBounds.min.y &&
        initial.localBounds.min.z==repeated.localBounds.min.z && initial.localBounds.max.x==repeated.localBounds.max.x &&
        initial.localBounds.max.y==repeated.localBounds.max.y && initial.localBounds.max.z==repeated.localBounds.max.z,
        "reused deformation retains exact output and bounds");

    scene.m_jointPaletteMatrices[0].positionMatrix=Math::Translation({3,0,0});
    const auto moved=prepare();
    Check(device.dispatches==initialDispatches+1 && moved.localBounds.min.x>2 && moved.geometryRevision!=initial.geometryRevision,"palette edit recomputes bounds and output");
    scene.m_entityMorphedMeshes[0]=*scene.Meshes()[0];
    for(auto& vertex:scene.m_entityMorphedMeshes[0].vertices)vertex.position.x+=2;
    const auto morphed=prepare();
    Check(device.dispatches==initialDispatches+2 && morphed.localBounds.min.x>4,"morph position edit recomputes output");
    scene.m_entityMorphedMeshes[0].vertices[0].normal.x=.5f;
    (void)prepare();
    Check(device.dispatches==initialDispatches+3,"morph normal edit must not reuse stale GPU vertices");
    scene.m_entityMorphedMeshes[0]={};
    scene.m_meshes[0]=std::make_shared<const MeshData>(CreateCubeMesh(2));
    (void)prepare();
    Check(device.dispatches==initialDispatches+4,"same mesh index with a different immutable owner must invalidate output");

    // Mimic the successful draw-history commit, then retain its buffer while another slot prepares.
    ModelRuntimeTestAccess::CommitHistory(model,scene);
    const auto historyBuffer=ModelRuntimeTestAccess::Draw(model).vertexBuffer;
    device.delayed=true;
    const auto next=prepare();
    Check(next.deformationHistoryValid && next.previousVertexBuffer==historyBuffer && next.vertexBuffer!=historyBuffer,
        "a distinct writable slot must preserve the previous deformation buffer");
    Check(prepare().vertexBuffer!=next.vertexBuffer,"in-flight deformation output must not be overwritten");
    device.Complete();device.delayed=false;
    model.InvalidateHistory();
    Check(!prepare().deformationHistoryValid,"explicit history invalidation must remain visible on reused output");
    scene.m_jointPaletteMatrices[0].positionMatrix=Math::Translation({6,0,0});device.failSubmit=true;
    Check(!ModelRuntimeTestAccess::Prepare(model,scene),"deformation submission rejection");
    const auto failedDispatches=device.dispatches;
    Check(ModelRuntimeTestAccess::Prepare(model,scene) && device.dispatches>failedDispatches,"failed output must be recomputed on retry");
}

void MixedDeformationReuse()
{
    ComputeDevice device;
    RendererConfig config;config.enableProfilerHud=false;config.enableHdrRendering=true;config.advanced.enabled=true;
    auto renderer=Renderer::Create(device,reinterpret_cast<void*>(1),config);
    Check(bool(renderer),"mixed model renderer fixture");
    ModelScene scene;
    Check(bool(scene.Add(CreateCubeMesh())) && bool(scene.Add(CreateCubeMesh())),"mixed shared mesh entities");
    scene.m_entityMeshes[1]=scene.m_entityMeshes[0];
    scene.m_meshSkinInfluences.resize(2);
    scene.m_meshSkinInfluences[0].resize(scene.Meshes()[0]->vertices.size());
    for(auto& influence:scene.m_meshSkinInfluences[0]) influence.weights.x=1;
    scene.m_jointPaletteMatrices.resize(2);scene.m_entitySkinPaletteOffsets={0,1};
    scene.m_entityMorphedMeshes.resize(2);scene.m_entityMorphedMeshes[1]=*scene.Meshes()[0];
    for(auto& vertex:scene.m_entityMorphedMeshes[1].vertices) vertex.position.x+=2;
    ModelRenderer model(*renderer);ModelRuntimeTestAccess::Shader(model);
    Check(ModelRuntimeTestAccess::Prepare(model,scene) && device.dispatches==2,"mixed initial deformation");
    const auto oldBounds=ModelRuntimeTestAccess::Draw(model).localBounds;
    scene.m_jointPaletteMatrices[0].positionMatrix=Math::Translation({4,0,0});
    Check(ModelRuntimeTestAccess::Prepare(model,scene) && device.dispatches==3,
        "unchanged morph follower must retain the shared input needed by a changed palette leader");
    Check(ModelRuntimeTestAccess::Draw(model).localBounds.min.x>oldBounds.max.x,
        "mixed deformation must apply the leader's changed palette");
}
}

int main(int argc,char** argv) try
{
    const std::string mode=argc>1?argv[1]:"animation";
    if(mode=="animation"){AnimationReuse();MorphPoseReuse();}
    else if(mode=="deformation")DeformationReuse();
    else if(mode=="aliases")RegistrationSnapshots();
    else if(mode=="controls")PlaybackControls();
    else if(mode=="mixed")MixedDeformationReuse();
    else return 2;
    std::cout<<"PASS model runtime "<<mode<<'\n';return 0;
}
catch(const std::exception& error)
{countAllocations=false;std::cerr<<error.what()<<'\n';return 1;}
