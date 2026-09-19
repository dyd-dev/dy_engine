#include "dyf.h"
#include "dyf/RHI.h"
#include "vertex.h"
#include "geometry.h"
#include "lighting.h"
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
    params.extra[2]=24;
    uint64_t frameLimit=0;
    bool framesSpecified=false,validation=false,fixedTime=false;
    float timeValue=0;
    std::string capture;
    for(int i=1;i<argc;i++) {
        std::string arg=argv[i];
        if(arg=="--help") {
            std::puts("DeferredLighting\n--mode lit|normal|albedo|position, --lights [0,64], --roughness [0.045,1], --metallic [0,1], --exposure [0.01,20]\n--frames N (0=unlimited), --capture path.ppm, --time SECONDS, --validation\n--capture defaults to 1 frames unless --frames is supplied.");
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
if(value=="lit") params.sizeTime[3]=0;
else if(value=="normal") params.sizeTime[3]=1;
else if(value=="albedo") params.sizeTime[3]=2;
else if(value=="position") params.sizeTime[3]=3;
else throw std::runtime_error("Unknown mode: "+value);
}else if(arg=="--lights") { double n=Number(value,0,64);if(std::floor(n)!=n) throw std::runtime_error("This option requires an integer.");params.extra[2]=float(n);}
else if(arg=="--roughness") { double n=Number(value,0.045,1);params.settings[1]=float(n);}
else if(arg=="--metallic") { double n=Number(value,0,1);params.settings[2]=float(n);}
else if(arg=="--exposure") { double n=Number(value,0.01,20);params.settings[0]=float(n);}

        else throw std::runtime_error("Unknown option: "+arg);
    }
    if(!capture.empty() && !framesSpecified) frameLimit=1;
    if(!capture.empty() && frameLimit==0) throw std::runtime_error("Capture requires a finite frame count.");
    dyf::Platform::Window window(960,640,"Advanced / DeferredLighting");
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
auto geometryShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::geometryEntryPoint,ShaderData::geometry,ShaderData::geometrySize}));
auto lightingShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::lightingEntryPoint,ShaderData::lighting,ShaderData::lightingSize}));
    SamplerDesc sampler;
    sampler.minFilter=SamplerFilter::Nearest;sampler.magFilter=SamplerFilter::Nearest;sampler.mipFilter=SamplerFilter::Nearest;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
        // Pipeline: geometry
    std::vector<ResourceBindingLayout> geometryBindings;
    std::array<ColorAttachmentDesc,3> geometryColors{};
    for(auto& attachment:geometryColors) attachment={Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All};
    GraphicsPipelineDesc geometryDesc;geometryDesc.vertexShader=vertexShader;geometryDesc.fragmentShader=geometryShader;
    geometryDesc.topology=PrimitiveTopology::TriangleList;geometryDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    geometryDesc.colorAttachments=geometryColors.data();geometryDesc.colorAttachmentCount=uint32_t(geometryColors.size());
    geometryDesc.layout={geometryBindings.data(),uint32_t(geometryBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto geometryPipeline=resources.Keep(device.CreateGraphicsPipeline(geometryDesc));
    // Pipeline: lighting
    std::vector<ResourceBindingLayout> lightingBindings;
    lightingBindings.push_back({0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    lightingBindings.push_back({1,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    lightingBindings.push_back({2,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    lightingBindings.push_back({8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
    std::array<ColorAttachmentDesc,1> lightingColors{};
    for(auto& attachment:lightingColors) attachment={Format::B8G8R8A8_UNORM,{},ColorWriteMask::All};
    GraphicsPipelineDesc lightingDesc;lightingDesc.vertexShader=vertexShader;lightingDesc.fragmentShader=lightingShader;
    lightingDesc.topology=PrimitiveTopology::TriangleList;lightingDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    lightingDesc.colorAttachments=lightingColors.data();lightingDesc.colorAttachmentCount=uint32_t(lightingColors.size());
    lightingDesc.layout={lightingBindings.data(),uint32_t(lightingBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto lightingPipeline=resources.Keep(device.CreateGraphicsPipeline(lightingDesc));


    std::unique_ptr<ResourceScope> imageResources;
    uint32_t imageWidth=0,imageHeight=0;
    bool initializeImages=true;
    TextureHandle positionBuffer=nullptr;
TextureHandle normalBuffer=nullptr;
TextureHandle albedoBuffer=nullptr;
ResourceSetHandle lightingSet=nullptr;

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
                        TextureDesc positionBufferDesc;positionBufferDesc.width=imageWidth;positionBufferDesc.height=imageHeight;
            positionBufferDesc.format=Format::R16G16B16A16_FLOAT;positionBufferDesc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            positionBuffer=imageResources->Keep(device.CreateTexture(positionBufferDesc));
            TextureDesc normalBufferDesc;normalBufferDesc.width=imageWidth;normalBufferDesc.height=imageHeight;
            normalBufferDesc.format=Format::R16G16B16A16_FLOAT;normalBufferDesc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            normalBuffer=imageResources->Keep(device.CreateTexture(normalBufferDesc));
            TextureDesc albedoBufferDesc;albedoBufferDesc.width=imageWidth;albedoBufferDesc.height=imageHeight;
            albedoBufferDesc.format=Format::R16G16B16A16_FLOAT;albedoBufferDesc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            albedoBuffer=imageResources->Keep(device.CreateTexture(albedoBufferDesc));

                        std::vector<ResourceBinding> lightingSetBindings;
            lightingSetBindings.push_back({0,0,nullptr,positionBuffer,0,0,{}});
            lightingSetBindings.push_back({1,0,nullptr,normalBuffer,0,0,{}});
            lightingSetBindings.push_back({2,0,nullptr,albedoBuffer,0,0,{}});
            lightingSet=imageResources->Keep(device.CreateResourceSet({lightingPipeline,lightingSetBindings.data(),uint32_t(lightingSetBindings.size())}));

        }
        params.sizeTime[0]=float(imageWidth);params.sizeTime[1]=float(imageHeight);
        params.sizeTime[2]=fixedTime?timeValue:float(frame)/60.0f;

        ResourceScope frameResources(device);
        auto* commands=frameResources.Keep(device.AcquireCommandList());
                if(initializeImages) {
            { const ResourceBarrierDesc barrier{nullptr,positionBuffer,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=positionBuffer;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,positionBuffer,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
            { const ResourceBarrierDesc barrier{nullptr,normalBuffer,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=normalBuffer;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,normalBuffer,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
            { const ResourceBarrierDesc barrier{nullptr,albedoBuffer,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=albedoBuffer;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,albedoBuffer,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
        }

                // 1. Populate three independent HDR G-buffer attachments.
        {
            const ResourceBarrierDesc positionBufferBarrier{nullptr,positionBuffer,ResourceState::ShaderResource,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&positionBufferBarrier,1);
            const ResourceBarrierDesc normalBufferBarrier{nullptr,normalBuffer,ResourceState::ShaderResource,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&normalBufferBarrier,1);
            const ResourceBarrierDesc albedoBufferBarrier{nullptr,albedoBuffer,ResourceState::ShaderResource,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&albedoBufferBarrier,1);
            std::array<ColorAttachment,3> colors{};
            colors[0].texture=positionBuffer;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            colors[1].texture=normalBuffer;colors[1].loadOp=LoadOp::Clear;colors[1].storeOp=StoreOp::Store;
            colors[2].texture=albedoBuffer;colors[2].loadOp=LoadOp::Clear;colors[2].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(geometryPipeline);
            commands->SetViewport({0,0,float(imageWidth),float(imageHeight),0,1});commands->SetScissor({0,0,imageWidth,imageHeight});
            commands->SetInlineConstants(0,sizeof(params),&params);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
            const ResourceBarrierDesc positionBufferReady{nullptr,positionBuffer,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&positionBufferReady,1);
            const ResourceBarrierDesc normalBufferReady{nullptr,normalBuffer,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&normalBufferReady,1);
            const ResourceBarrierDesc albedoBufferReady{nullptr,albedoBuffer,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&albedoBufferReady,1);
        }
        // 2. Deferred lighting reads world position, normal/roughness and albedo/metalness.
        {
            const ResourceBarrierDesc backbufferBarrier{nullptr,backbuffer,ResourceState::Present,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&backbufferBarrier,1);
            std::array<ColorAttachment,1> colors{};
            colors[0].texture=backbuffer;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(lightingPipeline);
            commands->BindResourceSet(lightingSet);
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
    std::fprintf(stderr,"DeferredLighting: %s\n",error.what());return 1;
}
