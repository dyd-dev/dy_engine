#include "RendererPerformanceSupport.h"
#include <dyf/Extends/Model.h>

using namespace RendererPerformance;
namespace
{
constexpr uint32_t Size=320;
constexpr unsigned Poses=6;
constexpr unsigned MaxFrameAttempts=32;
uint64_t skippedCalls=0;
void RetrySkippedFrame(Context& context, const char* operation, unsigned attempt)
{
    ++skippedCalls;
    std::printf("model frame skipped operation=%s attempt=%u/%u total_skips=%llu; draining before retry\n",
        operation,attempt+1,MaxFrameAttempts,static_cast<unsigned long long>(skippedCalls));
    Check(!context.device->IsLost(), "Native device lost while a Model frame was skipped");
    Check(context.device->WaitIdle(), "Model skipped-frame GPU/present drain failed");
    // Vulkan acquires with timeout zero; presentation/compositor image release
    // may lag queue completion. Let native messages and presentation progress.
    MSG message{};
    while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE))
    { TranslateMessage(&message); DispatchMessageW(&message); }
    Sleep(2);
}
struct Fixture
{
    dyf::ModelScene scene;
    dyf::ModelInstanceID instance=dyf::ModelInstanceID::Invalid;
    dyf::Camera camera;
    uint32_t clip=0;
    float duration=0;
    explicit Fixture(const std::string& path)
    {
        dyf::ModelSceneDesc desc;
        desc.path=path; desc.normalize=false;
        desc.transform=Math::Scaling(.012f)*Math::RotationX(1.5707963f)*Math::RotationY(3.14159265f);
        Check(dyf::AddModelToScene(scene,desc,&instance), "Fox.glb native fixture load failed");
        const auto* model=scene.TryGetModelInstance(instance);
        const auto* asset=model ? scene.TryGetModelAsset(model->assetId) : nullptr;
        Check(asset && !asset->animations.empty() && !asset->skins.empty(),
            "Native fixture requires animation clips and skinning inputs");
        clip=asset->animations.size()>1 ? 1u : 0u; // Fox Walk, where present.
        duration=asset->animations[clip].duration;
        Check(std::isfinite(duration) && duration>0, "Native fixture has no positive animation duration");
        Check(!scene.JointPaletteMatrices().empty(), "Native fixture did not build a joint palette");
        Check(camera.LookAt({2.6f,-3,1.8f},{0,0,.4f}) && camera.SetPerspective(1),
            "Fox native camera setup failed");
        std::printf("model fixture path=%s entities=%u palettes=%llu clip=%u duration=%.6f\n",
            path.c_str(),scene.GetEntityCount(),
            static_cast<unsigned long long>(scene.JointPaletteMatrices().size()),clip,duration);
    }
    void Pose(unsigned pose)
    {
        static constexpr std::array<float,Poses> fractions={0,.13f,.28f,.43f,.64f,.82f};
        Check(scene.PlayAnimation(instance,clip,false) && scene.UpdateAnimations(duration*fractions[pose%Poses]),
            "Native fixture animation pose update failed");
    }
};

std::vector<uint8_t> Capture(Context& context, dyf::ModelRenderer& model, const Fixture& fixture)
{
    dyf::Image image;
    bool captured=false;
    for(unsigned attempt=0;attempt<MaxFrameAttempts;++attempt)
    {
        const auto previous=context.renderer->GetLastRenderCompletion();
        Check(model.Render(fixture.scene,fixture.camera,&image), "Model native capture render failed");
        const auto completion=context.renderer->GetLastRenderCompletion();
        if(image.IsValid())
        {
            Check(bool(completion) && completion!=previous, "Model capture reused an earlier frame");
            captured=true; break;
        }
        Check(completion==previous, "Model submitted a frame but did not return captured pixels");
        RetrySkippedFrame(context,"capture",attempt);
    }
    Check(captured, "Model capture remained skipped after bounded native frame retries");
    Check(image.IsValid() && image.GetWidth()==Size && image.GetHeight()==Size,
        "Model native capture returned no pixels or skipped a hidden-window frame");
    NonBlank(image.GetPixels());
    const auto stats=context.renderer->GetLastRenderStatistics();
    Check(stats.mainDrawCount==fixture.scene.GetEntityCount() &&
        stats.mainInstanceCount==fixture.scene.GetEntityCount() && stats.culledEntityCount==0,
        "Model native custom draw path must retain each explicit entity");
    return image.GetPixels();
}
void Submit(Context& context, dyf::ModelRenderer& model, const Fixture& fixture)
{
    for(unsigned attempt=0;attempt<MaxFrameAttempts;++attempt)
    {
        const auto previous=context.renderer->GetLastRenderCompletion();
        Check(model.Render(fixture.scene,fixture.camera), "Model native queued render failed");
        const auto submitted=context.renderer->GetLastRenderCompletion();
        if(submitted && submitted!=previous) return;
        RetrySkippedFrame(context,"queued",attempt);
    }
    throw std::runtime_error("Model queued frame remained skipped after bounded native frame retries");
}

std::array<std::vector<uint8_t>,Poses> Exercise(Context& context, Fixture& fixture,
    dyf::SkinningExecutionMode mode, const char* label)
{
    std::array<std::vector<uint8_t>,Poses> reference;
    // Every oracle has entirely fresh Model influence/palette/output buffers.
    // The same Renderer/scene/camera isolate Model cache effects from raster state.
    for(unsigned pose=0;pose<Poses;++pose)
    {
        fixture.Pose(pose);
        dyf::ModelRenderer fresh(*context.renderer);
        Check(fresh.SetSkinningExecutionMode(mode), "Supported native skinning mode rejected");
        reference[pose]=Capture(context,fresh,fixture);
    }
    uint64_t changed=0;
    for(unsigned pose=1;pose<Poses;++pose) changed+=Compare(reference[0],reference[pose]).changedPixels;
    std::printf("model animation evidence mode=%s changed_pixels=%llu\n",label,
        static_cast<unsigned long long>(changed));
    Check(changed>100, "Selected animation poses did not produce visibly changing native geometry");

    {
        dyf::ModelRenderer reused(*context.renderer);
        Check(reused.SetSkinningExecutionMode(mode), "Native reused skinning mode rejected");
        for(unsigned pose=0;pose<Poses;++pose)
        {
            // Queue six successfully submitted changing poses without readback.
            // Skipped frame acquisition drains/retries the same pose explicitly;
            // skips never count as one of the six submitted samples.
            for(unsigned frame=0;frame<6;++frame)
            {
                fixture.Pose((pose+frame)%Poses);
                Submit(context,reused,fixture);
            }
            fixture.Pose(pose);
            const auto actual=Capture(context,reused,fixture);
            Equivalent((std::string(label)+"-reused-pose-"+std::to_string(pose)).c_str(),reference[pose],actual);
        }
        const auto warm=[&](unsigned frames) {
            for(unsigned frame=0;frame<frames;++frame)
            {
                fixture.Pose(frame%Poses);
                Submit(context,reused,fixture);
            }
            Check(context.device->WaitIdle(), "Model native allocation phase idle wait failed");
        };
        warm(30);
        const auto before=context.device->GetResourceAllocationCounters();
        warm(30);
        const auto after=context.device->GetResourceAllocationCounters();
        std::printf("model warm allocations mode=%s frames=30 buffers_created=%llu buffers_live_delta=%lld textures_created=%llu pipelines_created=%llu\n",
            label, static_cast<unsigned long long>(after.buffers.created-before.buffers.created),
            static_cast<long long>(after.buffers.live)-static_cast<long long>(before.buffers.live),
            static_cast<unsigned long long>(after.textures.created-before.textures.created),
            static_cast<unsigned long long>(after.pipelines.created-before.pipelines.created));
        Check(after.buffers.created==before.buffers.created && after.buffers.live==before.buffers.live,
            "Native changing-pose steady frames recreate or grow GPU buffers");
        Check(after.textures.created==before.textures.created && after.pipelines.created==before.pipelines.created,
            "Native changing-pose steady frames recreate textures/pipelines");
    }
    Check(context.device->WaitIdle(), "Model renderer teardown GPU idle failed");
    return reference;
}
#ifdef DY_LIVE_PROFILING_ENABLED
void OptimizationBypasses(Context& context,Fixture& fixture,dyf::SkinningExecutionMode mode)
{
    dyf::ModelRenderer model(*context.renderer);Check(model.SetSkinningExecutionMode(mode),"Native optimization skinning mode");unsigned pose=0;
    for(const auto* key:{"model.frameBufferReuse","model.skipUnchangedUploads","model.staticMeshCache","model.influenceCache"}){
        if(std::string(key)=="model.staticMeshCache"&&mode==dyf::SkinningExecutionMode::VertexShader){std::puts("model.staticMeshCache inactive in VS; compute fixture exercises this path");continue;}
        fixture.Pose(pose++);const auto reference=Capture(context,model,fixture);(void)Capture(context,model,fixture);
        const auto before=context.device->GetResourceAllocationCounters();const auto request=RequestBoolean(context,key,false,dyf::Diagnostics::FindLiveObject(&model));
        const auto actual=Capture(context,model,fixture);const auto off=AppliedBoolean(context,request,false);const auto after=context.device->GetResourceAllocationCounters();Equivalent(key,reference,actual);
        if(std::string(key)=="model.skipUnchangedUploads")Check(off.recorded.uploadCalls>0,"Model reference must issue logical uploads");else Check(after.buffers.created>before.buffers.created,"Model reference must allocate replacement buffers");
        const auto restore=RequestBoolean(context,key,true,dyf::Diagnostics::FindLiveObject(&model));Equivalent((std::string(key)+"-restored").c_str(),reference,Capture(context,model,fixture));(void)AppliedBoolean(context,restore,true);
        std::printf("native optimization mode=%s key=%s off_upload_calls=%llu off_upload_bytes=%llu off_created=%llu\n",mode==dyf::SkinningExecutionMode::VertexShader?"VS":"Compute",key,static_cast<unsigned long long>(off.recorded.uploadCalls),static_cast<unsigned long long>(off.recorded.uploadBytes),static_cast<unsigned long long>(off.recorded.objectsCreated));
    }
    Check(context.device->WaitIdle(),"Optimization Model native drain");std::puts("Model optimization control pixel contracts passed (live ON)");
}
#endif
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc>=2,"Usage: ModelResourceReuseNativeTests path/to/Fox.glb [--validation] [--optimization-only] [--vs-only|--compute-only]");
        bool validation=false,optimizationOnly=false,vsOnly=false,computeOnly=false;
        for(int i=2;i<argc;++i){const std::string argument=argv[i];if(argument=="--validation")validation=true;else if(argument=="--optimization-only")optimizationOnly=true;else if(argument=="--vs-only")vsOnly=true;else if(argument=="--compute-only")computeOnly=true;else throw std::runtime_error("Unknown Model native argument: "+argument);}
        Check(!(vsOnly&&computeOnly)&&(!(vsOnly||computeOnly)||optimizationOnly),"Mode selection requires --optimization-only and exactly one selected mode");
        Context context(Size,Size,true,false,validation);
        Fixture fixture(argv[1]);
        if(optimizationOnly){
#ifdef DY_LIVE_PROFILING_ENABLED
            if(!computeOnly)OptimizationBypasses(context,fixture,dyf::SkinningExecutionMode::VertexShader);
            if(!vsOnly)OptimizationBypasses(context,fixture,dyf::SkinningExecutionMode::ComputePreSkin);
            return 0;
#else
            throw std::runtime_error("--optimization-only requires DY_ENABLE_LIVE_PROFILING=ON");
#endif
        }
        const bool compute=context.device->Supports(RHI::Feature::Compute);
        std::printf("native Model supported modes: VertexShader=yes ComputePreSkin=%s\n",
            compute ? "yes" : "unsupported (not invoked)");
        const auto vertex=Exercise(context,fixture,dyf::SkinningExecutionMode::VertexShader,"VS");
        if(compute)
        {
            const auto computed=Exercise(context,fixture,dyf::SkinningExecutionMode::ComputePreSkin,"Compute");
            for(unsigned pose=0;pose<Poses;++pose)
                Equivalent(("VS-vs-Compute-pose-"+std::to_string(pose)).c_str(),vertex[pose],computed[pose]);
        }
        Check(context.device->WaitIdle(), "Final Model native GPU idle failed");
        context.renderer.reset();
        Check(context.device->WaitIdle(), "Final Renderer teardown GPU idle failed");
        const auto final=context.device->GetResourceAllocationCounters();
        Check(final.buffers.live==0 && final.textures.live==0 && final.pipelines.live==0,
            "Native Model/Renderer teardown left persistent tracked resources");
        std::printf("native Model total skipped acquisition calls=%llu (excluded from sample counts)\n",
            static_cast<unsigned long long>(skippedCalls));
        std::puts("Native Model reuse contracts passed: fresh-buffer pose oracle, queued changing palettes, steady allocations, supported skinning modes and teardown.");
        return 0;
    }
    catch(const std::exception& error)
    { std::fprintf(stderr,"Model native resource reuse contract failure: %s\n",error.what()); return 1; }
}
