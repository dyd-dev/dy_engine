#include "dyf/Camera.h"
#include "dyf/Extends/AdvancedRendering.h"
#include "dyf/Platform/Time.h"
#include "dyf/Platform/Window.h"
#include "dyf/Renderer.h"
#include "dyf/Scene.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace dyf;
namespace
{
constexpr float pi=3.14159265358979323846f;
MeshData Sphere()
{
    // 간소화할 수 있는 곡면 메시를 한 번 만들고 모든 객체가 공유한다.
    MeshData mesh;
    constexpr uint32_t rings=16,segments=24;
    for(uint32_t ring=0;ring<=rings;++ring) for(uint32_t segment=0;segment<=segments;++segment)
    {
        const float phi=pi*ring/rings,theta=2*pi*segment/segments;
        const Math::float3 n={std::sin(phi)*std::cos(theta),std::sin(phi)*std::sin(theta),std::cos(phi)};
        Vertex vertex;vertex.position=n;vertex.normal=n;
        vertex.uv={float(segment)/segments,float(ring)/rings};
        vertex.tangent={-std::sin(theta),std::cos(theta),0,-1};mesh.vertices.push_back(vertex);
    }
    for(uint32_t ring=0;ring<rings;++ring) for(uint32_t segment=0;segment<segments;++segment)
    {
        const auto a=ring*(segments+1)+segment,b=a+segments+1;
        if(ring) mesh.indices.insert(mesh.indices.end(),{a,b,a+1});
        if(ring+1<rings) mesh.indices.insert(mesh.indices.end(),{a+1,b,b+1});
    }
    return mesh;
}
}

int main(int argc,char** argv) try
{
    uint32_t objectCount=1600,frameLimit=0;
    bool validation=false;
    for(int i=1;i<argc;++i)
    {
        const std::string_view argument=argv[i];
        if(argument=="--help")
        {
            std::puts("AdvancedGPUCulling [--objects N] [--frames N] [--validation]\n"
                "GPU frustum + LOD + instance compaction + indexed indirect draws; no readback.\n"
                "4 frustum | U GPU/CPU | L LOD | O current-depth HiZ | Space pause | F11 profiler\n"
                "Default: 1600 moving objects. GPU visible counts are N/A; batches are CPU submissions.\n"
                "Compute/indirect unsupported: render the same scene with CPU culling.");
            return 0;
        }
        if(argument=="--validation") validation=true;
        else if((argument=="--objects" || argument=="--frames") && i+1<argc)
        {
            const std::string_view value=argv[++i];uint32_t number=0;
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || (argument=="--objects" && !number))
                throw std::runtime_error("Expected an unsigned integer; --objects must be positive.");
            (argument=="--objects"?objectCount:frameLimit)=number;
        }
        else throw std::runtime_error("Unknown or incomplete option; use --help.");
    }
    Platform::Window window(1280,720,"GPU Culling / 4 frustum | U GPU/CPU | L LOD | O HiZ | Space pause | F11 profiler");
    if(!window.GetHandle()) throw std::runtime_error("Window creation failed.");
    RHI::DeviceDesc deviceDesc;deviceDesc.enableValidation=validation;
    std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(deviceDesc));
    if(!device) throw std::runtime_error("Device creation failed.");
    if(!device->Supports(RHI::Feature::Rasterization))
    {
        std::fputs("UNSUPPORTED: this example needs a rasterizing backend.\n",stderr);return 77;
    }
    const bool gpuAvailable=device->Supports(RHI::Feature::Compute) && device->Supports(RHI::Feature::IndirectDraw);
    RendererConfig config;config.enableGpuCulling=gpuAvailable;config.enableLod=true;
    config.enableHdrRendering=true;config.advanced.enabled=true;
    config.advanced.ambientOcclusion=config.advanced.diffuseGI=config.advanced.reflections=false;
    config.advanced.temporalAA=config.advanced.bloom=false;
    config.lighting.shadows=false;config.lighting.ambientIntensity=.12f;
    config.clearColor={.025f,.035f,.06f,1};config.enableProfilerHud=true;config.profilerStartsExpanded=true;
    auto renderer=Renderer::Create(*device,window.GetHandle(),config);
    if(!renderer) throw std::runtime_error("Renderer creation failed.");
    if(!renderer->AddExtension(std::make_unique<dyf::AdvancedRendering>()))
        throw std::runtime_error("Advanced rendering extension registration failed.");
    if(!gpuAvailable) std::fputs("GPU culling unavailable: Compute and IndirectDraw are required; using CPU culling.\n",stderr);
    Scene scene;const auto mesh=Sphere();
    const std::array<Math::float4,4> colors={Math::float4{.08f,.5f,.9f,1},{.95f,.34f,.09f,1},
        {.15f,.8f,.4f,1},{.7f,.2f,.85f,1}};
    std::array<EntityHandle,4> sources;
    std::vector<EntityHandle> objects;objects.reserve(objectCount);
    for(uint32_t i=0;i<objectCount;++i)
    {
        const auto kind=static_cast<uint32_t>(uint64_t(i)*colors.size()/objectCount);
        MaterialDesc material;material.baseColor=colors[kind];material.roughnessFactor=.42f;material.metallicFactor=.15f;
        auto entity=sources[kind]?scene.AddInstance(sources[kind]):scene.Add(mesh,material);
        if(!entity) throw std::runtime_error("Cannot create the object grid.");
        if(!sources[kind]) sources[kind]=entity;
        objects.push_back(entity);
    }
    const uint32_t side=static_cast<uint32_t>(std::ceil(std::sqrt(double(objectCount))));
    MaterialDesc floor;floor.baseColor={.14f,.17f,.21f,1};floor.roughnessFactor=.9f;
    const float span=2.6f*side+12;
    if(!scene.Add(CreateCubeMesh(),floor,Math::Translation({0,0,-.45f})*Math::Scaling({span,span,.4f})))
        throw std::runtime_error("Cannot create the floor.");
    DirectionalLight sun;sun.direction={.45f,.3f,.85f};sun.intensity=3;sun.castShadow=false;
    if(!scene.Add(sun)) throw std::runtime_error("Cannot create the light.");
    std::fprintf(stderr,"%u shared-mesh objects | 4 frustum, U GPU/CPU, L LOD, O HiZ, Space pause, F11 profiler.\n"
        "GPU visible counts are N/A. The profiler lists the actual GPU graph passes.\n",objectCount);
    Camera camera;Platform::Time clock;
    double time=0,reportTime=0;uint32_t submitted=0,reportFrames=0;
    bool paused=false,ready=false;
    while(window.IsRunning() && (!frameLimit || submitted<frameLimit))
    {
        window.PollEvents();const auto& input=window.GetInput();
        if(input.WasPressed(Platform::Key::Escape)) window.RequestClose();
        if(!window.IsRunning()) break;
        const auto extent=window.GetFramebufferSize();
        const bool suspended=window.IsMinimized() || extent.width<=0 || extent.height<=0 || (ready && !window.HasFocus());
        const double dt=clock.Tick(suspended);
        if(suspended) {Platform::Window::WaitEvents();continue;}
        if(input.WasPressed(Platform::Key::Space)) paused=!paused;
        config=renderer->GetConfig();bool changed=false;
        if(input.WasPressed(Platform::Key::Num4)) {config.enableFrustumCulling=!config.enableFrustumCulling;changed=true;}
        if(input.WasPressed(Platform::Key::L)) {config.enableLod=!config.enableLod;changed=true;}
        if(input.WasPressed(Platform::Key::U))
        {
            if(gpuAvailable) {config.enableGpuCulling=!config.enableGpuCulling;changed=true;}
            else std::fputs("GPU culling unavailable; CPU fallback remains active.\n",stderr);
        }
        if(input.WasPressed(Platform::Key::O))
        {
            if(gpuAvailable) {config.enableOcclusionCulling=!config.enableOcclusionCulling;changed=true;}
            else std::fputs("HiZ requires the GPU visibility path.\n",stderr);
        }
        if(changed)
        {
            if(!renderer->SetConfig(config)) throw std::runtime_error("Renderer settings rejected.");
            std::fprintf(stderr,"path=%s frustum=%s LOD=%s HiZ=%s\n",config.enableGpuCulling?"GPU":"CPU",
                config.enableFrustumCulling?"on":"off",config.enableLod?"on":"off",
                config.enableGpuCulling && config.enableOcclusionCulling?"on":"off");
        }
        if(ready && !paused) time+=dt;
        const float t=float(time);
        for(uint32_t i=0;i<objectCount;++i)
        {
            const float phase=float(i)*.37f;
            const Math::float3 position={(float(i%side)-float(side-1)*.5f)*2.6f+.45f*std::sin(t+phase),
                (float(i/side)-float(side-1)*.5f)*2.6f+.45f*std::cos(t*.8f+phase),1.4f+.65f*std::sin(t*1.2f+phase)};
            const auto transform=Math::Translation(position)*Math::RotationZ(t*.4f+phase)*Math::Scaling({.65f,.8f,.75f});
            if(!objects[i].SetTransform(transform)) throw std::runtime_error("Object transform rejected.");
        }
        if(!camera.LookAt({22*std::sin(t*.13f),22*std::cos(t*.13f),12},{0,0,1}) ||
            !camera.SetPerspective(float(extent.width)/extent.height,pi/3,.1f,std::max(160.f,side*3.5f)))
            throw std::runtime_error("Camera setup failed.");
        const auto previous=renderer->GetLastRenderCompletion();
        if(!renderer->Render(scene,camera)) throw std::runtime_error("Rendering failed.");
        const auto completion=renderer->GetLastRenderCompletion();
        if(completion && completion!=previous) {++submitted;++reportFrames;}
        if(!ready && completion) {ready=true;clock.Reset();}
        reportTime+=dt;
        if(reportTime>=1)
        {
            const auto& statistics=renderer->GetLastRenderStatistics();
            if(statistics.gpuVisibility)
                std::fprintf(stderr,"GPU %.1f submitted/s | indirect batches %u | visible objects/triangles N/A\n",reportFrames/reportTime,statistics.indirectBatchCount);
            else std::fprintf(stderr,"CPU %.1f submitted/s | draws %u | visible objects %u | culled %u\n",reportFrames/reportTime,
                statistics.mainDrawCount,statistics.mainInstanceCount,statistics.culledEntityCount);
            reportTime=0;reportFrames=0;
        }
        if(completion==previous) Platform::Window::WaitEvents(.001);
    }
    return 0;
}
catch(const std::exception& error)
{
    std::fprintf(stderr,"AdvancedGPUCulling: %s\n",error.what());return 1;
}
