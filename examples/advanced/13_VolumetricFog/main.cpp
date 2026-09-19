#include "dyf.h"
#include "dyf/RHI.h"
#include "vertex.h"
#include "scene.h"
#include "fog.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dyf::RHI;
namespace {
// Four complete 16-byte vectors: GLSL push constants, HLSL b15, Metal buffer(15).
struct alignas(16) Params {
    float sizeTime[4] = {960,640,0,0};
    float settings[4] = {1,0.45f,0.8f,0.9f};
    float jitter[4] = {};
    float extra[4] = {};
};
static_assert(sizeof(Params)==64 && alignof(Params)==16);
static_assert(offsetof(Params,settings)==16 && offsetof(Params,jitter)==32 && offsetof(Params,extra)==48);
double Number(const std::string& value,double minimum,double maximum) {
    size_t consumed=0; double result=std::stod(value,&consumed);
    if(consumed!=value.size() || !std::isfinite(result) || result<minimum || result>maximum)
        throw std::runtime_error("Numeric option out of range: "+value);
    return result;
}
void Capture(IDevice& device,TextureHandle texture,const std::string& path) {
    TextureReadback image;
    if(!device.ReadTexture(texture,image)) throw std::runtime_error("ReadTexture failed.");
    std::ofstream stream(path,std::ios::binary);
    if(!stream) throw std::runtime_error("Cannot open capture path.");
    stream<<"P6\n"<<image.width<<" "<<image.height<<"\n255\n";
    const bool bgra=image.format==Format::B8G8R8A8_UNORM || image.format==Format::B8G8R8A8_UNORM_SRGB;
    for(uint32_t y=0;y<image.height;y++) for(uint32_t x=0;x<image.width;x++) {
        const auto* pixel=image.pixels.data()+size_t(y)*image.rowPitch+size_t(x)*4;
        const char rgb[3]={char(pixel[bgra?2:0]),char(pixel[1]),char(pixel[bgra?0:2])};
        stream.write(rgb,3);
    }
    if(!stream) throw std::runtime_error("Capture write failed.");
    std::printf("Captured %s (%ux%u).\n",path.c_str(),image.width,image.height);
}

}

int main(int argc,char** argv) try {
    Params params;
    params.extra[0]=0.22f;params.extra[1]=64;params.settings[1]=0.7f;
    uint64_t frameLimit=0;
    bool framesSpecified=false,validation=false,fixedTime=false;
    float timeValue=0;
    std::string capture;
    for(int i=1;i<argc;i++) {
        std::string arg=argv[i];
        if(arg=="--help") {
            std::puts("VolumetricFog\n--mode fog|off|transmittance|scattering, --density [0,3], --steps [8,256], --height-falloff [0,4], --exposure [0.01,20]\nProcedural single scattering; no volumetric shadow map or froxel history.\n--frames N (0=unlimited), --capture path.ppm, --time SECONDS, --validation\n--capture defaults to 1 frames unless --frames is supplied.");
            return 0;
        }
        if(arg=="--validation") {validation=true;continue;}
        if(i+1>=argc) throw std::runtime_error("Missing value for "+arg);
        std::string value=argv[++i];
        if(arg=="--frames") {
            double n=Number(value,0,10000000);
            if(std::floor(n)!=n) throw std::runtime_error("Frame count must be an integer.");
            frameLimit=uint64_t(n);framesSpecified=true;
        } else if(arg=="--capture") capture=value;
        else if(arg=="--time") {timeValue=float(Number(value,0,1000000));fixedTime=true;}
        else if(arg=="--mode") {
if(value=="fog") params.sizeTime[3]=0;
else if(value=="off") params.sizeTime[3]=1;
else if(value=="transmittance") params.sizeTime[3]=2;
else if(value=="scattering") params.sizeTime[3]=3;
else throw std::runtime_error("Unknown mode: "+value);
}else if(arg=="--density") { double n=Number(value,0,3);params.extra[0]=float(n);}
else if(arg=="--steps") { double n=Number(value,8,256);if(std::floor(n)!=n) throw std::runtime_error("This option requires an integer.");params.extra[1]=float(n);}
else if(arg=="--height-falloff") { double n=Number(value,0,4);params.settings[1]=float(n);}
else if(arg=="--exposure") { double n=Number(value,0.01,20);params.settings[0]=float(n);}

        else throw std::runtime_error("Unknown option: "+arg);
    }
    if(!capture.empty() && !framesSpecified) frameLimit=1;
    if(!capture.empty() && frameLimit==0) throw std::runtime_error("Capture requires a finite frame count.");
    dyf::Platform::Window window(960,640,"Advanced / VolumetricFog");
    if(!window.GetHandle()) throw std::runtime_error("Window creation failed.");
    DeviceDesc deviceDesc;deviceDesc.enableValidation=validation;
    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));
    if(!owner) throw std::runtime_error("Device creation failed.");
    auto& device=*owner;

    ResourceScope resources(device);
    SwapchainDesc swap;swap.window=window.GetHandle();swap.format=Format::B8G8R8A8_UNORM;
    swap.minimumImageCount=2;swap.presentMode=PresentMode::Fifo;swap.allowReadback=!capture.empty();
    if(!device.CreateSwapchain(swap)) throw std::runtime_error("Swapchain creation failed.");
    auto vertexShader=resources.Keep(device.CreateShader({ShaderStage::Vertex,ShaderData::vertexEntryPoint,ShaderData::vertex,ShaderData::vertexSize}));
auto sceneShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::sceneEntryPoint,ShaderData::scene,ShaderData::sceneSize}));
auto fogShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::fogEntryPoint,ShaderData::fog,ShaderData::fogSize}));
    SamplerDesc sampler;
    sampler.minFilter=SamplerFilter::Linear;sampler.magFilter=SamplerFilter::Linear;sampler.mipFilter=SamplerFilter::Nearest;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
        // Pipeline: scene
    std::vector<ResourceBindingLayout> sceneBindings;
    std::array<ColorAttachmentDesc,1> sceneColors{};
    for(auto& attachment:sceneColors) attachment={Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All};
    GraphicsPipelineDesc sceneDesc;sceneDesc.vertexShader=vertexShader;sceneDesc.fragmentShader=sceneShader;
    sceneDesc.topology=PrimitiveTopology::TriangleList;sceneDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    sceneDesc.colorAttachments=sceneColors.data();sceneDesc.colorAttachmentCount=uint32_t(sceneColors.size());
    sceneDesc.layout={sceneBindings.data(),uint32_t(sceneBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto scenePipeline=resources.Keep(device.CreateGraphicsPipeline(sceneDesc));
    // Pipeline: fog
    std::vector<ResourceBindingLayout> fogBindings;
    fogBindings.push_back({0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    fogBindings.push_back({8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
    std::array<ColorAttachmentDesc,1> fogColors{};
    for(auto& attachment:fogColors) attachment={Format::B8G8R8A8_UNORM,{},ColorWriteMask::All};
    GraphicsPipelineDesc fogDesc;fogDesc.vertexShader=vertexShader;fogDesc.fragmentShader=fogShader;
    fogDesc.topology=PrimitiveTopology::TriangleList;fogDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    fogDesc.colorAttachments=fogColors.data();fogDesc.colorAttachmentCount=uint32_t(fogColors.size());
    fogDesc.layout={fogBindings.data(),uint32_t(fogBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto fogPipeline=resources.Keep(device.CreateGraphicsPipeline(fogDesc));


    std::unique_ptr<ResourceScope> imageResources;
    uint32_t imageWidth=0,imageHeight=0;
    bool initializeImages=true;
    TextureHandle sceneDepth=nullptr;
ResourceSetHandle fogSet=nullptr;

    uint64_t frame=0;
    while(!frameLimit || frame<frameLimit) {
        window.PollEvents();if(!window.IsRunning()) break;
        if(!device.BeginFrame()) {
            if(device.IsLost()) throw std::runtime_error("Device lost.");
            continue;
        }
        auto backbuffer=device.GetBackBuffer();
        if(!backbuffer) throw std::runtime_error("Backbuffer unavailable.");
        const auto dimensions=backbuffer->GetDesc();
        if(dimensions.width!=imageWidth || dimensions.height!=imageHeight) {
            if(!device.WaitIdle()) throw std::runtime_error("Resize wait failed.");
            imageResources=std::make_unique<ResourceScope>(device);
            imageWidth=dimensions.width;imageHeight=dimensions.height;initializeImages=true;
                        TextureDesc sceneDepthDesc;sceneDepthDesc.width=imageWidth;sceneDepthDesc.height=imageHeight;
            sceneDepthDesc.format=Format::R16G16B16A16_FLOAT;sceneDepthDesc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            sceneDepth=imageResources->Keep(device.CreateTexture(sceneDepthDesc));

                        std::vector<ResourceBinding> fogSetBindings;
            fogSetBindings.push_back({0,0,nullptr,sceneDepth,0,0,{}});
            fogSet=imageResources->Keep(device.CreateResourceSet({fogPipeline,fogSetBindings.data(),uint32_t(fogSetBindings.size())}));

        }
        params.sizeTime[0]=float(imageWidth);params.sizeTime[1]=float(imageHeight);
        params.sizeTime[2]=fixedTime?timeValue:float(frame)/60.0f;

        ResourceScope frameResources(device);
        auto* commands=frameResources.Keep(device.AcquireCommandList());
                if(initializeImages) {
            { const ResourceBarrierDesc barrier{nullptr,sceneDepth,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=sceneDepth;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,sceneDepth,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
        }

                // 1. Opaque scene color and linear ray distance in an HDR target.
        {
            const ResourceBarrierDesc sceneDepthBarrier{nullptr,sceneDepth,ResourceState::ShaderResource,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&sceneDepthBarrier,1);
            std::array<ColorAttachment,1> colors{};
            colors[0].texture=sceneDepth;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(scenePipeline);
            commands->SetViewport({0,0,float(imageWidth),float(imageHeight),0,1});commands->SetScissor({0,0,imageWidth,imageHeight});
            commands->SetInlineConstants(0,sizeof(params),&params);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
            const ResourceBarrierDesc sceneDepthReady{nullptr,sceneDepth,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&sceneDepthReady,1);
        }
        // 2. Integrate height/noise density only before the nearest opaque surface.
        {
            const ResourceBarrierDesc backbufferBarrier{nullptr,backbuffer,ResourceState::Present,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&backbufferBarrier,1);
            std::array<ColorAttachment,1> colors{};
            colors[0].texture=backbuffer;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(fogPipeline);
            commands->BindResourceSet(fogSet);
            commands->SetViewport({0,0,float(imageWidth),float(imageHeight),0,1});commands->SetScissor({0,0,imageWidth,imageHeight});
            commands->SetInlineConstants(0,sizeof(params),&params);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
        }

        const ResourceBarrierDesc presentBarrier{nullptr,backbuffer,ResourceState::RenderTarget,ResourceState::Present,{}};
        commands->ResourceBarrier(&presentBarrier,1);
        if(!commands->Close() || !device.Submit(&commands,1)) throw std::runtime_error("Frame recording/submission failed.");
        // CPU state is committed only after Submit succeeds; GPU execution remains ordered on this queue.
        initializeImages=false;

        ++frame;
        if(!capture.empty() && frame==frameLimit) Capture(device,backbuffer,capture);
        if(!device.Present()) throw std::runtime_error("Present failed.");
    }
    if(!device.WaitIdle()) throw std::runtime_error("Final GPU wait failed.");
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"VolumetricFog: %s\n",error.what());return 1;
}
