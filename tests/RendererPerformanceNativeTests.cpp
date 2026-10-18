#include "RendererMotionSupport.h"
#include <functional>

using namespace RendererPerformance;
namespace
{
constexpr uint32_t Size = 256;
unsigned fixtureTick=0;
// Boundary/material/shadow contracts sample the same moving trajectory at three
// ticks. Both optimization settings render the identical pose at each tick.
void MoveFixture(dyf::Scene& scene,float amount=.035f)
{
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        auto entity=scene.GetEntity(static_cast<dyf::EntityID>(i));
        Check(entity.SetTransform(Math::Translation({0,amount*float(fixtureTick+1),0})*
            entity.GetTransform()*Math::RotationZ(.01f*float(fixtureTick+1))),"Fixture motion update failed");
    }
}
void SetOptimized(Context& context, bool value)
{
    auto config=context.renderer->GetConfig();
    config.enableInstancing=value; config.enableFrustumCulling=value;
    Check(context.renderer->SetConfig(config), "Optimization settings rejected");
}
std::vector<uint8_t> Capture(Context& context, const dyf::Scene& scene, const dyf::Camera& camera)
{
    Target target(*context.device,Size,Size);
    target.Render(*context.renderer,scene,camera);
    Wait(*context.device,target.completion);
    return target.Capture();
}
#ifdef DY_LIVE_PROFILING_ENABLED
void OptimizationBypasses(Context& context)
{
    dyf::Scene scene;dyf::MaterialDesc material;material.baseColor={.7f,.8f,.6f,1};material.baseColorTexture=dyf::Image(1,1,{190,220,240,255});
    auto entity=scene.Add(dyf::CreateCubeMesh(.65f),material);Check(bool(entity),"Optimization fixture mesh");
    for(unsigned i=0;i<5;++i){auto copy=scene.AddInstance(entity,Math::Translation({float(i)-2,0,0}));Check(bool(copy),"Optimization fixture instance");}
    Check(context.renderer->SetConfig(Configuration(true)),"Optimization fixture config");const auto camera=TopCamera();
    for(const auto* key:{"optimization.transformUploads","optimization.resourceSetCache","optimization.bindlessMaterials"}){
        const auto reference=Capture(context,scene,camera);NonBlank(reference);(void)Capture(context,scene,camera);
        if(std::string(key)=="optimization.bindlessMaterials"&&!BooleanAvailable(context,key))continue;
        const auto request=RequestBoolean(context,key,false);const auto actual=Capture(context,scene,camera);const auto off=AppliedBoolean(context,request,false);Equivalent(key,reference,actual);
        if(std::string(key)=="optimization.transformUploads")Check(off.recorded.uploadBytes>=scene.GetEntityCount()*sizeof(Math::float4x4),"Full transform reference must upload all transforms");
        if(std::string(key)=="optimization.resourceSetCache")Check(off.recorded.objectsCreated>0,"Resource-set reference must create actual resources");
        const auto restore=RequestBoolean(context,key,true);Equivalent((std::string(key)+"-restored").c_str(),reference,Capture(context,scene,camera));(void)AppliedBoolean(context,restore,true);
        std::printf("native optimization key=%s off_upload_calls=%llu off_upload_bytes=%llu off_created=%llu\n",key,static_cast<unsigned long long>(off.recorded.uploadCalls),static_cast<unsigned long long>(off.recorded.uploadBytes),static_cast<unsigned long long>(off.recorded.objectsCreated));
        Check(entity.SetPosition({.13f,0,.07f}),"Optimization moving pose");
    }
    Check(context.device->WaitIdle(),"Optimization Renderer native drain");std::puts("Renderer optimization control pixel contracts passed (live ON)");
}
#endif
void Counts(const dyf::RendererStatistics& stats, uint32_t draws, uint32_t instances, uint32_t culled)
{
    std::printf("counts draws=%u instances=%u culled=%u (expected %u/%u/%u)\n",
        stats.mainDrawCount,stats.mainInstanceCount,stats.culledEntityCount,draws,instances,culled);
    Check(stats.mainDrawCount==draws && stats.mainInstanceCount==instances &&
        stats.culledEntityCount==culled, "Main draw/instance/culling counts differ from fixture");
}
dyf::EntityHandle Instance(dyf::Scene& scene, const dyf::EntityHandle& source, const Math::float4x4& transform)
{
    const auto handle=scene.AddInstance(source,transform);
    Check(bool(handle), "AddInstance failed for valid source/transform");
    return handle;
}

void TransformsAndCulling(Context& context)
{
    dyf::Scene scene;
    dyf::MaterialDesc material; material.baseColor={.6f,.8f,.35f,1}; material.roughnessFactor=.32f;
    material.normalTexture=dyf::Image(1,1,{190,100,235,255},dyf::ColorSpace::Linear);
    const auto source=scene.Add(dyf::CreateCubeMesh(.6f),material,Math::Translation({-2,-1,0}));
    Check(bool(source), "Source entity creation failed");
    Instance(scene,source,Math::Translation({-1,-1,0})*Math::RotationZ(.47f));
    Instance(scene,source,Math::Translation({0,-1,0})*Math::Scaling({.4f,1.2f,.7f}));
    Instance(scene,source,Math::Translation({1,-1,0})*Math::RotationY(.61f)*Math::Scaling({-1,.7f,1.1f}));
    Instance(scene,source,Math::Translation({-1,1,0})*Math::RotationX(.32f)*Math::Scaling({1,-.5f,1.5f}));
    Instance(scene,source,Math::Translation({1,1,0})*Math::Scaling({.8f,1.3f,-.6f}));
    // Center is outside, but mesh extends across x=4. This must be retained.
    Instance(scene,source,Math::Translation({4.1f,0,0}));
    Instance(scene,source,Math::Translation({40,0,0}));
    Instance(scene,source,Math::Translation({0,0,10})); // behind the eye at z=8
    dyf::DirectionalLight light; light.direction={.4f,.3f,1}; light.castShadow=false;
    Check(bool(scene.Add(light)), "Directional light creation failed");
    MoveFixture(scene);
    auto config=context.renderer->GetConfig(); config.lighting.enabled=true; config.lighting.shadows=false;
    Check(context.renderer->SetConfig(config), "Lit fixture config failed");
    const auto camera=TopCamera();
    SetOptimized(context,false);
    const auto baseline=Capture(context,scene,camera);
    Counts(context.renderer->GetLastRenderStatistics(),9,9,0); NonBlank(baseline);
    SetOptimized(context,true);
    const auto optimized=Capture(context,scene,camera);
    Counts(context.renderer->GetLastRenderStatistics(),1,7,2);
    Equivalent("rotated/nonuniform/reflected/boundary",baseline,optimized);
    // Also compare culling and instancing switches independently.
    config=context.renderer->GetConfig(); config.enableInstancing=false;
    Check(context.renderer->SetConfig(config), "Culling-only settings failed");
    Equivalent("culling-only",baseline,Capture(context,scene,camera));
    Counts(context.renderer->GetLastRenderStatistics(),7,7,2);
    config=context.renderer->GetConfig(); config.enableInstancing=true; config.enableFrustumCulling=false;
    Check(context.renderer->SetConfig(config), "Instancing-only settings failed");
    Equivalent("instancing-only",baseline,Capture(context,scene,camera));
    Counts(context.renderer->GetLastRenderStatistics(),1,9,0);
    auto skewed=camera;
    skewed.projection.m[4]+=.08f;
    skewed.projection.m[8]+=.03f;
    SetOptimized(context,false);
    const auto skewedBaseline=Capture(context,scene,skewed);
    SetOptimized(context,true);
    Equivalent("nonstandard-skewed-projection",skewedBaseline,Capture(context,scene,skewed));
}

void IndependentMaterial(Context& context)
{
    dyf::Scene scene;
    dyf::MaterialDesc material; material.baseColor={.7f,.7f,.7f,1};
    const auto source=scene.Add(dyf::CreateCubeMesh(.9f),material,Math::Translation({-1.5f,0,0}));
    auto copy=Instance(scene,source,Math::Translation({0,0,0}));
    Instance(scene,source,Math::Translation({1.5f,0,0}));
    MoveFixture(scene);
    const auto camera=TopCamera(5);
    SetOptimized(context,true);
    const auto unchanged=Capture(context,scene,camera);
    Counts(context.renderer->GetLastRenderStatistics(),1,3,0);
    auto edited=copy.GetMaterial(); edited.baseColor={.08f,.15f,.9f,1};
    edited.baseColorTexture=dyf::Image(1,1,{70,230,255,255});
    Check(copy.SetMaterial(edited),"Independent copied material edit failed");
    Check(source.GetMaterial().baseColor.x==material.baseColor.x &&
        !source.GetMaterial().baseColorTexture.IsValid(), "Copied material edit mutated source");
    SetOptimized(context,false);
    const auto baseline=Capture(context,scene,camera);
    Counts(context.renderer->GetLastRenderStatistics(),3,3,0);
    Check(Compare(unchanged,baseline).changedPixels>200, "Material edit did not visibly affect fixture");
    SetOptimized(context,true);
    Equivalent("independent-material-texture",baseline,Capture(context,scene,camera));
    const auto split=context.renderer->GetLastRenderStatistics();
    Check(split.mainInstanceCount==3 && split.mainDrawCount>=2 && split.mainDrawCount<=3,
        "Different effective material failed to split batches");
    // The center projected entity changes; outer source pixels must stay intact.
    const size_t row=size_t(Size/2)*Size*4;
    const size_t left=row+size_t(Size/5)*4;
    Check(Compare(std::vector<uint8_t>(unchanged.begin()+left,unchanged.begin()+left+4),
        std::vector<uint8_t>(baseline.begin()+left,baseline.begin()+left+4)).max<=2,
        "Editing the center copy changed the source's visible pixel");
}

void NearPlane(Context& context)
{
    dyf::Scene scene;
    dyf::MaterialDesc material; material.baseColor={.2f,.9f,.6f,1};
    // A forward-facing triangle genuinely crosses the z=-.1 near plane.
    // Unlike an axis-aligned cube whose entire visible front face is clipped,
    // two deeper vertices retain a large visible clipped trapezoid.
    dyf::MeshData mesh;
    for(const Math::float3 position : {Math::float3{-.04f,-.04f,-.16f},
        Math::float3{.04f,-.04f,-.16f}, Math::float3{0,.04f,-.05f}})
    {
        dyf::Vertex vertex;
        vertex.position=position; vertex.normal={0,0,1}; vertex.uv={0,0};
        mesh.vertices.push_back(vertex);
    }
    mesh.indices={0,1,2};
    const auto source=scene.Add(mesh,material,Math::Translation({-.045f,0,0}));
    Check(bool(source), "Near-plane source creation failed");
    Instance(scene,source,Math::Translation({.045f,0,0}));
    Instance(scene,source,Math::Translation({0,0,.5f}));
    MoveFixture(scene,.001f);
    dyf::Camera camera;
    Check(camera.LookAt({0,0,0},{0,0,-1},{0,1,0}) && camera.SetPerspective(1,1.2f,.1f,10),
        "Near-plane camera setup failed");
    // This fixture isolates clipping/culling, independently of prior lighting.
    Check(context.renderer->SetConfig(Configuration(false)), "Near-plane unlit config failed");
    SetOptimized(context,false);
    const auto baseline=Capture(context,scene,camera); NonBlank(baseline);
    SetOptimized(context,true);
    Equivalent("near-plane-intersection",baseline,Capture(context,scene,camera));
    Counts(context.renderer->GetLastRenderStatistics(),1,2,1);
}

void OffCameraShadow(Context& context)
{
    dyf::Scene scene;
    dyf::MaterialDesc ground; ground.baseColor={.7f,.7f,.7f,1}; ground.roughnessFactor=1;
    auto plane=scene.Add(dyf::CreateCubeMesh(1),ground,
        Math::Translation({0,0,-.15f})*Math::Scaling({4,4,.2f}));
    Check(bool(plane), "Shadow receiver creation failed");
    auto lighting=plane.GetLighting(); lighting.castShadow=false;
    Check(plane.SetLighting(lighting), "Shadow receiver settings failed");
    auto caster=scene.Add(dyf::CreateCubeMesh(.7f),ground,Math::Translation({2.7f,0,2}));
    Check(bool(caster), "Off-camera shadow caster creation failed");
    dyf::DirectionalLight light; light.direction={1,0,1}; light.intensity=2; light.shadowStrength=1;
    Check(bool(scene.Add(light)), "Shadow light creation failed");
    MoveFixture(scene);
    const auto camera=TopCamera(4);
    auto config=context.renderer->GetConfig(); config.lighting.enabled=true; config.lighting.shadows=true;
    config.lighting.ambientIntensity=.01f; config.lighting.environment.diffuseIntensity=0;
    Check(context.renderer->SetConfig(config), "Shadow fixture config failed");
    SetOptimized(context,false);
    const auto baseline=Capture(context,scene,camera); NonBlank(baseline);
    SetOptimized(context,true);
    Equivalent("off-camera-shadow-caster",baseline,Capture(context,scene,camera));
    Counts(context.renderer->GetLastRenderStatistics(),1,1,1);
    lighting=caster.GetLighting(); lighting.castShadow=false;
    Check(caster.SetLighting(lighting), "Disable caster shadow failed");
    const auto without=Capture(context,scene,camera);
    const auto evidence=Compare(baseline,without);
    std::printf("shadow evidence pixels=%llu max=%u mean=%.6f\n",
        static_cast<unsigned long long>(evidence.changedPixels),evidence.max,evidence.mean);
    Check(evidence.changedPixels>100 && evidence.max>12,
        "Off-camera caster did not produce a visible shadow; fixture cannot prove shadow retention");
    Check(context.renderer->SetConfig(Configuration(true)), "Restore unlit settings failed");
}

struct ChangingScene
{
    dyf::Scene scene;
    dyf::EntityHandle source, copy;
    std::array<dyf::Image,3> colors={dyf::Image(1,1,{230,40,20,255}),
        dyf::Image(1,1,{20,230,50,255}),dyf::Image(1,1,{30,60,230,255})};
    ChangingScene()
    {
        source=scene.Add(dyf::CreateCubeMesh(.8f));
        Check(bool(source), "Changing source creation failed");
        copy=Instance(scene,source,Math::Translation({1.5f,0,0}));
    }
    void Set(unsigned frame)
    {
        auto material=source.GetMaterial(); material.baseColorTexture=colors[frame%3];
        material.baseColor={1,1,1,1};
        Check(source.SetMaterial(material), "Changing texture assignment failed");
        Check(source.SetTransform(Math::Translation({float(frame)*.18f-1.2f,-.7f,0})*
            Math::RotationZ(float(frame)*.17f)), "Changing transform assignment failed");
        material.baseColor={.3f+.08f*frame,.8f,.5f,1};
        Check(copy.SetMaterial(material) && copy.SetPosition({1.2f-float(frame)*.1f,.7f,0}),
            "Changing independent copy assignment failed");
    }
};
void ChangingFramesInFlight(Context& context)
{
    constexpr unsigned frames=6;
    ChangingScene fixture;
    const auto camera=TopCamera(5);
    std::array<std::vector<uint8_t>,frames> expected;
    SetOptimized(context,false);
    for(unsigned frame=0;frame<frames;++frame)
    {
        fixture.Set(frame); expected[frame]=Capture(context,fixture.scene,camera); NonBlank(expected[frame]);
        if(frame) Check(Compare(expected[frame-1],expected[frame]).changedPixels>100,
            "Changing-frame references are not visually distinct");
    }
    SetOptimized(context,true);
    std::array<std::unique_ptr<Target>,frames> targets;
    // No diagnostic readback or explicit CPU fence wait during this submission burst.
    // Six separate outputs allow checking every earlier frame after all six submits.
    for(unsigned frame=0;frame<frames;++frame)
    {
        targets[frame]=std::make_unique<Target>(*context.device,Size,Size);
        fixture.Set(frame); targets[frame]->Render(*context.renderer,fixture.scene,camera);
        Counts(context.renderer->GetLastRenderStatistics(),2,2,0);
    }
    Wait(*context.device,targets.back()->completion);
    for(unsigned frame=0;frame<frames;++frame)
        Equivalent(("in-flight-frame-"+std::to_string(frame)).c_str(),expected[frame],targets[frame]->Capture());
}

void SteadyAllocations(Context& context)
{
    MovingWorkload scene(1024);
    SetOptimized(context,true);
    const auto camera=scene.Camera();
    std::array<std::unique_ptr<Target>,3> targets;
    for(auto& target:targets) target=std::make_unique<Target>(*context.device,Size,Size);
    unsigned tick=0;
    const auto run=[&](unsigned frames) {
        for(unsigned i=0;i<frames;++i)
        {
            auto& target=*targets[i%3];
            if(target.completion) Wait(*context.device,target.completion);
            scene.Update(++tick); scene.VerifyFrame(tick);
            target.Render(*context.renderer,scene,camera);
        }
        for(auto& target:targets) Wait(*context.device,target->completion);
    };
    run(60);
    const auto first=targets.back()->Capture();
    const auto before=context.device->GetResourceAllocationCounters();
    const auto uploadBefore=context.device->GetUploadMemoryStatistics();
    run(120);
    const auto after=context.device->GetResourceAllocationCounters();
    const auto uploadAfter=context.device->GetUploadMemoryStatistics();
    std::printf("moving warm allocation frames=120 entities=1024 buffers=%llu textures=%llu pipelines=%llu live_buffers=%lld upload_pages=%llu upload_reuses=%llu upload_bytes=%llu\n",
        static_cast<unsigned long long>(after.buffers.created-before.buffers.created),
        static_cast<unsigned long long>(after.textures.created-before.textures.created),
        static_cast<unsigned long long>(after.pipelines.created-before.pipelines.created),
        static_cast<long long>(after.buffers.live)-static_cast<long long>(before.buffers.live),
        static_cast<unsigned long long>(uploadAfter.allocationCount-uploadBefore.allocationCount),
        static_cast<unsigned long long>(uploadAfter.reuseCount-uploadBefore.reuseCount),
        static_cast<unsigned long long>(uploadAfter.requestedBytes-uploadBefore.requestedBytes));
    Check(after.buffers.created==before.buffers.created, "Warm moving frames create equivalent GPU buffers");
    Check(after.textures.created==before.textures.created && after.pipelines.created==before.pipelines.created,
        "Warm moving frames create textures or pipelines");
    Check(after.buffers.live==before.buffers.live, "Warm moving frames grow live buffers");
    Check(uploadAfter.allocationCount==uploadBefore.allocationCount && uploadAfter.reuseCount>uploadBefore.reuseCount,
        "Warm moving uploads must reuse existing pages");
    Check(uploadAfter.requestedBytes-uploadBefore.requestedBytes>=uint64_t(120)*1024*64,
        "Moving frame transform uploads were incorrectly skipped");
    Counts(context.renderer->GetLastRenderStatistics(),1,1024,0);
    const auto last=targets.back()->Capture(); NonBlank(last);
    Check(Compare(first,last).changedPixels>100,"Moving allocation fixture rendered stale transforms");
    for(unsigned pose : {1u,17u,39u,63u,89u,119u})
    {
        scene.Update(pose); scene.VerifyFrame(pose);
        SetOptimized(context,false); const auto reference=Capture(context,scene,camera);
        SetOptimized(context,true); const auto actual=Capture(context,scene,camera);
        Equivalent(("all-moving-1024-tick-"+std::to_string(pose)).c_str(),reference,actual);
    }
}

void ExplicitDrawFallback(Context& context)
{
    dyf::Scene scene;
    const auto source=scene.Add(dyf::CreateCubeMesh(.8f));
    Check(bool(source), "Fallback fixture source failed");
    Instance(scene,source,Math::Translation({1.2f,0,0}));
    Instance(scene,source,Math::Translation({40,0,0}));
    MoveFixture(scene);
    const auto camera=TopCamera(5);
    std::vector<dyf::RendererDrawDesc> draws(scene.GetEntityCount());
    dyf::Image baseline,optimized;
    SetOptimized(context,false);
    Check(context.renderer->Render(scene,camera,draws,nullptr,&baseline) && baseline.IsValid(),
        "Explicit draw baseline render/readback failed or frame skipped");
    Counts(context.renderer->GetLastRenderStatistics(),3,3,0); NonBlank(baseline.GetPixels());
    SetOptimized(context,true);
    Check(context.renderer->Render(scene,camera,draws,nullptr,&optimized) && optimized.IsValid(),
        "Explicit draw optimized render/readback failed or frame skipped");
    Counts(context.renderer->GetLastRenderStatistics(),3,3,0);
    Equivalent("explicit-draw-conservative-fallback",baseline.GetPixels(),optimized.GetPixels());
}

void HdrToggleAndResize(Context& context)
{
    dyf::Scene scene;
    dyf::MaterialDesc material; material.baseColor={.55f,.28f,.13f,1}; material.emissiveColor={1.1f,.2f,.05f};
    const auto source=scene.Add(dyf::CreateCubeMesh(.8f),material,Math::Translation({-.9f,0,0}));
    Check(bool(source), "HDR fixture source failed");
    Instance(scene,source,Math::Translation({.9f,0,0})*Math::RotationZ(.4f));
    const auto camera=TopCamera(5);
    constexpr unsigned frames=6;
    const std::array<uint32_t,frames> widths={256,192,320,160,256,224};
    const std::array<uint32_t,frames> heights={256,288,192,160,256,320};
    const std::array<bool,frames> hdr={false,true,true,false,true,false};
    std::array<std::vector<uint8_t>,frames> reference;
    const auto configure=[&](unsigned frame,bool optimized) {
        for(uint32_t i=0;i<scene.GetEntityCount();++i)
            Check(scene.GetEntity(static_cast<dyf::EntityID>(i)).SetTransform(
                Math::Translation({i==0 ? -.9f : .9f,.03f*float(frame+1),0})*
                Math::RotationZ(.1f*float(frame+1)+.4f*float(i))),"HDR fixture motion update failed");
        auto config=Configuration(optimized);
        config.enableHdrRendering=hdr[frame]; config.exposure=.75f+.13f*frame;
        Check(context.renderer->SetConfig(config), "HDR/resize fixture settings failed");
    };
    for(unsigned frame=0;frame<frames;++frame)
    {
        configure(frame,false);
        Target target(*context.device,widths[frame],heights[frame]);
        target.Render(*context.renderer,scene,camera);
        Wait(*context.device,target.completion);
        reference[frame]=target.Capture(); NonBlank(reference[frame]);
        Counts(context.renderer->GetLastRenderStatistics(),2,2,0);
    }
    std::array<std::unique_ptr<Target>,frames> targets;
    for(unsigned frame=0;frame<frames;++frame)
    {
        configure(frame,true);
        targets[frame]=std::make_unique<Target>(*context.device,widths[frame],heights[frame]);
        targets[frame]->Render(*context.renderer,scene,camera);
        Counts(context.renderer->GetLastRenderStatistics(),1,2,0);
    }
    Wait(*context.device,targets.back()->completion);
    for(unsigned frame=0;frame<frames;++frame)
        Equivalent(("hdr-toggle-resize-frame-"+std::to_string(frame)).c_str(),reference[frame],targets[frame]->Capture());
    Check(context.renderer->SetConfig(Configuration(true)), "HDR fixture restore settings failed");
}
}
int main(int argc,char** argv)
{
    try
    {
        bool validation=false,optimizationOnly=false;
        for(int i=1;i<argc;++i)
        {const std::string argument=argv[i];if(argument=="--validation")validation=true;else if(argument=="--optimization-only")optimizationOnly=true;else throw std::runtime_error("Unknown native test option: "+argument);}
        Context context(Size,Size,false,false,validation);
        if(optimizationOnly){
#ifdef DY_LIVE_PROFILING_ENABLED
            OptimizationBypasses(context);return 0;
#else
            throw std::runtime_error("--optimization-only requires DY_ENABLE_LIVE_PROFILING=ON");
#endif
        }
        for(fixtureTick=0;fixtureTick<3;++fixtureTick)
        {
            std::printf("moving renderer fixture tick=%u\n",fixtureTick+1);
            TransformsAndCulling(context);
            IndependentMaterial(context);
            NearPlane(context);
            OffCameraShadow(context);
            ExplicitDrawFallback(context);
        }
        ChangingFramesInFlight(context);
        SteadyAllocations(context);
        HdrToggleAndResize(context);
        Check(context.device->WaitIdle(), "Final GPU idle wait failed");
        context.renderer.reset();
        Check(context.device->WaitIdle(), "Renderer teardown wait failed");
        const auto final=context.device->GetResourceAllocationCounters();
        Check(final.buffers.live==0 && final.textures.live==0 && final.pipelines.live==0,
            "Renderer/target teardown left live resources (swapchain images are excluded)");
        std::puts("Renderer native contracts passed: lit transforms, independent materials, frustum boundaries, shadows, changing in-flight snapshots, allocations, explicit draw fallback and teardown.");
        return 0;
    }
    catch(const std::exception& error)
    { std::fprintf(stderr,"Renderer native performance contract failure: %s\n",error.what()); return 1; }
}
