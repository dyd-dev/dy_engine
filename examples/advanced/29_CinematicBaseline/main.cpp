#include "dyf.h"
#include "dyf/RHI.h"
#include <dyf/Extends/Model.h>
#include "../../CameraPath.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(ENABLE_METAL)
#include "CanvasLibrary.h"
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
#include "CanvasVertex.h"
#include "CanvasFragment.h"
#endif

namespace
{
namespace RHI=dyf::RHI;
using Clock=std::chrono::steady_clock;
constexpr uint32_t renderWidth=1920,renderHeight=1080;
constexpr float pi=3.14159265358979323846f;

void Check(bool condition,const char* message)
{
    if(!condition)throw std::runtime_error(message);
}

uint64_t Unsigned(const std::string& text)
{
    size_t consumed=0;
    Check(!text.empty() && text.front()!='-',"Expected a nonnegative integer.");
    const auto value=std::stoull(text,&consumed);
    Check(consumed==text.size(),"Invalid integer option.");
    return value;
}

double Number(const std::string& text)
{
    size_t consumed=0;
    const double value=std::stod(text,&consumed);
    Check(consumed==text.size() && std::isfinite(value) && value>=0,"Expected a finite nonnegative number.");
    return value;
}

void Help()
{
    std::puts("AdvancedCinematicBaseline: stock renderer at commit 89ac61c\n"
        "  --scene FILE   FBX, glTF, GLB or OBJ environment; default original Sponza glTF/PNG\n"
        "  --path FILE    Shared seconds,eyeXYZ,targetXYZ,fovDegrees,upXYZ camera CSV\n"
        "  --start N --paused --width N --height N\n"
        "  --up y|z --exposure N --budget-mib N --max-animation-keys N\n"
        "  Space pause/resume | R restart | Left/Right seek 5 s | Home/End first/last\n"
        "  M print submitted counts | H help | Escape close\n"
        "  Fixed 1920x1080 scene output, centered aspect-preserving presentation.\n"
        "  No AdvancedRendering modules, AA, HDRI, screen GI/reflections or raster glass.\n"
        "  Source FBX lights, explicit ORM interpretation and normal Y convention\n"
        "  cannot be selected through this revision's ModelLoadOptions.\n"
        "  This revision cannot decode DDS textures; Bistro's DDS materials are unsupported.");
}

// 표시만 구형 RHI로 조합한다. 장면의 재질·조명·톤 매핑은 구형 Renderer 그대로다.
class Presentation
{
public:
    explicit Presentation(RHI::IDevice& device):m_device(device),m_resources(device)
    {
        RHI::ShaderDesc vertex,fragment;
#if defined(ENABLE_METAL)
        vertex={RHI::ShaderStage::Vertex,"canvasVertex",kCanvasLibrary,kCanvasLibrarySize};
        fragment={RHI::ShaderStage::Fragment,"canvasFragment",kCanvasLibrary,kCanvasLibrarySize};
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
        vertex={RHI::ShaderStage::Vertex,"main",kCanvasVertex,kCanvasVertexSize};
        fragment={RHI::ShaderStage::Fragment,"main",kCanvasFragment,kCanvasFragmentSize};
#else
        throw std::runtime_error("CinematicBaseline requires Vulkan, D3D12 or Metal.");
#endif
        auto vs=m_resources.Keep(device.CreateShader(vertex));
        auto ps=m_resources.Keep(device.CreateShader(fragment));
        RHI::TextureDesc texture;
        texture.width=renderWidth;texture.height=renderHeight;
        texture.format=RHI::Format::B8G8R8A8_UNORM;
        texture.usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
        target=m_resources.Keep(device.CreateTexture(texture));
        Check(vs && ps && target,"Presentation shader/target creation failed.");

        RHI::SamplerDesc sampler;
        sampler.minFilter=sampler.magFilter=RHI::SamplerFilter::Linear;
        sampler.mipFilter=RHI::SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=RHI::SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
        const RHI::ResourceBindingLayout bindings[]={
            {0,RHI::ResourceBindingType::SampledTexture,1,RHI::ShaderStageFlags::Fragment,{}},
            {1,RHI::ResourceBindingType::StaticSampler,1,RHI::ShaderStageFlags::Fragment,sampler}};
        const RHI::VertexBufferLayout input{0,sizeof(Vertex),RHI::VertexStepMode::Vertex};
        const RHI::VertexAttribute attributes[]={
            {0,0,RHI::Format::R32G32_FLOAT,0},{1,0,RHI::Format::R32G32_FLOAT,8},
            {2,0,RHI::Format::R32G32B32A32_FLOAT,16}};
        const RHI::ColorAttachmentDesc color{texture.format,{},RHI::ColorWriteMask::All};
        RHI::GraphicsPipelineDesc desc;
        desc.vertexShader=vs;desc.fragmentShader=ps;desc.topology=RHI::PrimitiveTopology::TriangleList;
        desc.raster={RHI::FillMode::Solid,RHI::CullMode::None,RHI::FrontFace::CounterClockwise,0,0,0};
        desc.vertexBuffers=&input;desc.vertexBufferCount=1;
        desc.vertexAttributes=attributes;desc.vertexAttributeCount=3;
        desc.colorAttachments=&color;desc.colorAttachmentCount=1;
        desc.layout={bindings,2,32,RHI::ShaderStageFlags::Vertex|RHI::ShaderStageFlags::Fragment,15};
        m_pipeline=m_resources.Keep(device.CreateGraphicsPipeline(desc));
        Check(m_pipeline!=nullptr,"Presentation pipeline creation failed.");
        const RHI::ResourceBinding image{0,0,nullptr,target,0,0,{}};
        m_set=m_resources.Keep(device.CreateResourceSet({m_pipeline,&image,1}));

        const std::array<Vertex,6> vertices={{{-1,1,0,0},{-1,-1,0,1},{1,1,1,0},
            {1,1,1,0},{-1,-1,0,1},{1,-1,1,1}}};
        m_vertices=m_resources.Keep(device.CreateBuffer({sizeof(vertices),sizeof(Vertex),
            RHI::BufferUsage::Vertex,RHI::ResourceState::CopyDestination}));
        Check(m_set && m_vertices,"Presentation resources unavailable.");
        RHI::ResourceScope upload(device);
        auto* commands=upload.Keep(device.AcquireCommandList());
        Check(commands!=nullptr,"Presentation upload command list unavailable.");
        Check(device.UpdateBuffer(*commands,m_vertices,0,vertices.data(),sizeof(vertices)),"Presentation vertex upload failed.");
        const RHI::ResourceBarrierDesc ready{m_vertices,nullptr,RHI::ResourceState::CopyDestination,RHI::ResourceState::VertexBuffer,{}};
        commands->ResourceBarrier(&ready,1);
        Check(commands->Close() && device.Submit(&commands,1),"Presentation vertex submission failed.");
    }

    bool Present()
    {
        if(!m_device.BeginFrame())
        {
            Check(!m_device.IsLost(),"Device lost while acquiring the window.");
            return false;
        }
        auto backbuffer=m_device.GetBackBuffer();
        Check(backbuffer!=nullptr,"Backbuffer unavailable.");
        const auto& output=backbuffer->GetDesc();
        Check(output.format==RHI::Format::B8G8R8A8_UNORM,"Unexpected baseline swapchain format.");
        double width=output.width,height=output.height;
        if(uint64_t(output.width)*renderHeight<=uint64_t(output.height)*renderWidth)
            height=double(output.width)*renderHeight/renderWidth;
        else width=double(output.height)*renderWidth/renderHeight;
        const auto x=static_cast<int32_t>(std::floor((output.width-width)*.5));
        const auto y=static_cast<int32_t>(std::floor((output.height-height)*.5));
        RHI::ResourceScope frame(m_device);
        auto* commands=frame.Keep(m_device.AcquireCommandList());
        Check(commands!=nullptr,"Presentation command list unavailable.");
        const RHI::ResourceBarrierDesc begin{nullptr,backbuffer,RHI::ResourceState::Present,RHI::ResourceState::RenderTarget,{}};
        commands->ResourceBarrier(&begin,1);
        RHI::ColorAttachment color;
        color.texture=backbuffer;color.loadOp=RHI::LoadOp::Clear;color.storeOp=RHI::StoreOp::Store;color.clearColor[3]=1;
        commands->BeginRendering({&color,1,nullptr});
        commands->BindGraphicsPipeline(m_pipeline);commands->BindResourceSet(m_set);
        commands->BindVertexBuffer(0,m_vertices,0);
        commands->SetViewport({float(x),float(y),float(width),float(height),0,1});
        commands->SetScissor({x,y,uint32_t(std::ceil(width)),uint32_t(std::ceil(height))});
        // UNORM에 완성된 표시색을 복사한다. Canvas의 sRGB 왕복을 상쇄하며 노출은 재적용하지 않는다.
        const float transform[8]={1,1,0,0,1,0,0,0};
        commands->SetInlineConstants(0,sizeof(transform),transform);
        commands->DrawInstanced(6,1,0,0);commands->EndRendering();
        const RHI::ResourceBarrierDesc end{nullptr,backbuffer,RHI::ResourceState::RenderTarget,RHI::ResourceState::Present,{}};
        commands->ResourceBarrier(&end,1);
        Check(commands->Close() && m_device.Submit(&commands,1),"Window copy submission failed.");
        Check(m_device.Present(),"Window presentation failed.");
        return true;
    }

    RHI::TextureHandle target=nullptr;
    RHI::ResourceState state=RHI::ResourceState::Undefined;

private:
    struct Vertex {float x,y,u,v;float r=1,g=1,b=1,a=1;};
    RHI::IDevice& m_device;
    RHI::ResourceScope m_resources;
    RHI::BufferHandle m_vertices=nullptr;
    RHI::PipelineHandle m_pipeline=nullptr;
    RHI::ResourceSetHandle m_set=nullptr;
};
}

int main(int argc,char** argv)try
{
    std::string scenePath="/home/suhan/downloads/main_sponza/NewSponza_Main_glTF_003.gltf";
    std::string cameraPath=DY_BASELINE_CAMERA_PATH;
    uint32_t width=renderWidth,height=renderHeight;
    uint64_t budget=8192ull*1024*1024,maxAnimationKeys=std::numeric_limits<uint64_t>::max();
    double start=0;float exposure=1.0f;bool paused=false,yUp=true;
    for(int i=1;i<argc;++i)
    {
        const std::string arg=argv[i];
        if(arg=="--help"){Help();return 0;}
        if(arg=="--paused"){paused=true;continue;}
        Check(i+1<argc,"Missing option value; use --help.");
        const std::string value=argv[++i];
        if(arg=="--scene")scenePath=value;
        else if(arg=="--path")cameraPath=value;
        else if(arg=="--start")start=Number(value);
        else if(arg=="--up"){Check(value=="y" || value=="z","--up requires y or z.");yUp=value=="y";}
        else if(arg=="--width" || arg=="--height")
        {
            const auto pixels=Unsigned(value);
            Check(pixels>0 && pixels<=uint64_t(std::numeric_limits<int>::max()),"Invalid window dimension.");
            (arg=="--width"?width:height)=static_cast<uint32_t>(pixels);
        }
        else if(arg=="--budget-mib")
        {
            const auto mib=Unsigned(value);
            Check(mib>0 && mib<=std::numeric_limits<uint64_t>::max()/(1024*1024),"Invalid byte budget.");
            budget=mib*1024*1024;
        }
        else if(arg=="--max-animation-keys")maxAnimationKeys=Unsigned(value);
        else if(arg=="--exposure")
        {
            const auto number=Number(value);
            Check(number<=std::numeric_limits<float>::max(),"Exposure exceeds float range.");
            exposure=static_cast<float>(number);
        }
        else throw std::runtime_error("Unknown option: "+arg);
    }

    showcase::CameraPath path;
    path.Load(cameraPath);
    dyf::ModelScene scene;
    dyf::ModelSceneDesc model;
    model.path=scenePath;model.normalizedSize=80;
    model.transform=yUp?dyf::Math::RotationX(pi*.5f):dyf::Math::float4x4::Identity();
    model.loadOptions.maxSourceBytes=model.loadOptions.maxParserInputBytes=budget;
    model.loadOptions.maxParserBytes=model.loadOptions.maxDecodedBytes=budget;
    model.loadOptions.maxAnimationKeys=maxAnimationKeys;
    std::fprintf(stderr,"Legacy engine: 89ac61c | source %s | normalize80/Z-up | fixed %ux%u\n",scenePath.c_str(),renderWidth,renderHeight);
    std::fprintf(stderr,"Default comparison uses original Sponza glTF/PNG: this revision has no DDS decoder, so Bistro DDS materials are not a supported baseline.\n");
    std::fprintf(stderr,"Legacy model import: original textures/geometry; no imported lights/ORM switch/normal-Y override. FBX bake rate remains %.1f Hz.\n",model.loadOptions.fbxBakeRate);
    std::fprintf(stderr,"Loading environment; decoded/parser/source budget %llu MiB, animation-key budget %llu.\n",
        static_cast<unsigned long long>(budget/(1024*1024)),static_cast<unsigned long long>(maxAnimationKeys));
    Check(dyf::AddModelToScene(scene,model),"Legacy environment loading failed.");
    dyf::DirectionalLight sun;
    sun.direction={-.38f,-.55f,.74f};sun.color={1,.91f,.78f};sun.intensity=3.5f;sun.shadowStrength=.85f;
    Check(bool(scene.Add(sun)),"Directional light creation failed.");
    std::fprintf(stderr,"Loaded %zu meshes, %zu materials, %u entities. Camera CSV: %s (%.3f s).\n",
        scene.Meshes().size(),scene.Materials().size(),scene.GetEntityCount(),cameraPath.c_str(),path.Duration());

    dyf::Platform::Window window(width,height,"Cinematic baseline / stock 89ac61c");
    Check(window.GetHandle()!=nullptr,"Window creation failed.");
    dyf::RendererConfig config;
    config.enableHdrRendering=true;config.exposure=exposure;config.enableProfilerHud=false;
    config.clearColor={.025f,.035f,.055f,1};
    config.lighting.shadows=true;config.lighting.shadowQuality=dyf::ShadowQuality::High;
    auto renderer=dyf::Renderer::Create(window.GetHandle(),config);
    Check(bool(renderer),"Legacy renderer creation failed.");
    Presentation output(renderer->GetDevice());
    Help();
    dyf::Camera camera;
    double seconds=std::clamp(start,0.0,path.Duration());
    bool ready=false;
    auto previous=Clock::now();
    while(window.IsRunning())
    {
        window.PollEvents();
        if(!window.IsRunning())break;
        const auto now=Clock::now();
        const double elapsed=std::chrono::duration<double>(now-previous).count();previous=now;
        const auto size=window.GetFramebufferSize();
        if(size.width<=0 || size.height<=0 || window.IsMinimized() || (ready && !window.HasFocus()))
        {
            window.WaitEvents(.05);
            continue;
        }
        const auto& input=window.GetInput();
        using dyf::Platform::Key;
        if(input.WasPressed(Key::Escape))break;
        if(input.WasPressed(Key::Space))paused=!paused;
        if(ready && !paused)seconds=std::min(seconds+elapsed,path.Duration());
        if(input.WasPressed(Key::R) || input.WasPressed(Key::Home))seconds=0;
        if(input.WasPressed(Key::End))seconds=path.Duration();
        if(input.WasPressed(Key::Left))seconds=std::max(0.0,seconds-5);
        if(input.WasPressed(Key::Right))seconds=std::min(path.Duration(),seconds+5);
        if(input.WasPressed(Key::H))Help();
        if(input.WasPressed(Key::M))
        {
            const auto& stats=renderer->GetLastRenderStatistics();
            std::printf("Baseline %.3f/%.3f s %s | submitted draws %u, instances %u, CPU-culled %u\n",
                seconds,path.Duration(),paused?"paused":"tour",stats.mainDrawCount,stats.mainInstanceCount,stats.culledEntityCount);
        }
        const auto pose=path.Sample(seconds);
        Check(camera.LookAt(pose.eye,pose.target,pose.up) &&
            camera.SetPerspective(float(renderWidth)/renderHeight,pose.fov*pi/180,.03f,160),"Invalid camera CSV pose.");
        Check(renderer->RenderToTexture(scene,camera,output.target,output.state),"Legacy scene frame failed.");
        output.state=RHI::ResourceState::ShaderResource;
        const bool presented=output.Present();
        // 첫 업로드 시간은 동선에 더하지 않고 실제 창 표시 뒤부터 재생한다.
        if(presented && !ready) previous=Clock::now();
        ready=ready || presented;
        if(!presented) window.WaitEvents(.01);
    }
    return 0;
}
catch(const std::exception& error)
{
    std::fprintf(stderr,"CinematicBaseline: %s\n",error.what());
    return 1;
}
