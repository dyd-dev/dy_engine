#include "dyf.h"
#include "dyf/RHI.h"
#include "vertex.h"
#include "scene.h"
#include "resolve.h"
#include "present.h"
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
float Halton(uint64_t index,uint32_t base) {
    float result=0,f=1;
    while(index) { f/=float(base);result+=f*float(index%base);index/=base; }
    return result;
}
}

int main(int argc,char** argv) try {
    Params params;

    uint64_t frameLimit=0;
    bool framesSpecified=false,validation=false,fixedTime=false;
    float timeValue=0;
    std::string capture;
    for(int i=1;i<argc;i++) {
        std::string arg=argv[i];
        if(arg=="--help") {
            std::puts("TemporalAA\n--mode taa|off|unclamped|velocity, --history-weight [0,0.99]\nMoving 2D pattern; fixed --time freezes motion, while sampling jitter continues.\n--frames N (0=unlimited), --capture path.ppm, --time SECONDS, --validation\n--capture defaults to 32 frames unless --frames is supplied.");
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
if(value=="taa") params.sizeTime[3]=0;
else if(value=="off") params.sizeTime[3]=1;
else if(value=="unclamped") params.sizeTime[3]=2;
else if(value=="velocity") params.sizeTime[3]=3;
else throw std::runtime_error("Unknown mode: "+value);
}else if(arg=="--history-weight") { double n=Number(value,0,0.99);params.settings[3]=float(n);}

        else throw std::runtime_error("Unknown option: "+arg);
    }
    if(!capture.empty() && !framesSpecified) frameLimit=32;
    if(!capture.empty() && frameLimit==0) throw std::runtime_error("Capture requires a finite frame count.");
    dyf::Platform::Window window(960,640,"Advanced / TemporalAA");
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
auto resolveShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::resolveEntryPoint,ShaderData::resolve,ShaderData::resolveSize}));
auto presentShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::presentEntryPoint,ShaderData::present,ShaderData::presentSize}));
    SamplerDesc sampler;
    sampler.minFilter=SamplerFilter::Linear;sampler.magFilter=SamplerFilter::Linear;sampler.mipFilter=SamplerFilter::Nearest;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
        // Pipeline: scene
    std::vector<ResourceBindingLayout> sceneBindings;
    std::array<ColorAttachmentDesc,2> sceneColors{};
    for(auto& attachment:sceneColors) attachment={Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All};
    GraphicsPipelineDesc sceneDesc;sceneDesc.vertexShader=vertexShader;sceneDesc.fragmentShader=sceneShader;
    sceneDesc.topology=PrimitiveTopology::TriangleList;sceneDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    sceneDesc.colorAttachments=sceneColors.data();sceneDesc.colorAttachmentCount=uint32_t(sceneColors.size());
    sceneDesc.layout={sceneBindings.data(),uint32_t(sceneBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto scenePipeline=resources.Keep(device.CreateGraphicsPipeline(sceneDesc));
    // Pipeline: resolve
    std::vector<ResourceBindingLayout> resolveBindings;
    resolveBindings.push_back({0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    resolveBindings.push_back({1,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    resolveBindings.push_back({2,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    resolveBindings.push_back({8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
    std::array<ColorAttachmentDesc,1> resolveColors{};
    for(auto& attachment:resolveColors) attachment={Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All};
    GraphicsPipelineDesc resolveDesc;resolveDesc.vertexShader=vertexShader;resolveDesc.fragmentShader=resolveShader;
    resolveDesc.topology=PrimitiveTopology::TriangleList;resolveDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    resolveDesc.colorAttachments=resolveColors.data();resolveDesc.colorAttachmentCount=uint32_t(resolveColors.size());
    resolveDesc.layout={resolveBindings.data(),uint32_t(resolveBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto resolvePipeline=resources.Keep(device.CreateGraphicsPipeline(resolveDesc));
    // Pipeline: present
    std::vector<ResourceBindingLayout> presentBindings;
    presentBindings.push_back({0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    presentBindings.push_back({1,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    presentBindings.push_back({8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
    std::array<ColorAttachmentDesc,1> presentColors{};
    for(auto& attachment:presentColors) attachment={Format::B8G8R8A8_UNORM,{},ColorWriteMask::All};
    GraphicsPipelineDesc presentDesc;presentDesc.vertexShader=vertexShader;presentDesc.fragmentShader=presentShader;
    presentDesc.topology=PrimitiveTopology::TriangleList;presentDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    presentDesc.colorAttachments=presentColors.data();presentDesc.colorAttachmentCount=uint32_t(presentColors.size());
    presentDesc.layout={presentBindings.data(),uint32_t(presentBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto presentPipeline=resources.Keep(device.CreateGraphicsPipeline(presentDesc));


    std::unique_ptr<ResourceScope> imageResources;
    uint32_t imageWidth=0,imageHeight=0;
    bool initializeImages=true;
    TextureHandle currentColor=nullptr;
TextureHandle velocity=nullptr;
TextureHandle history0=nullptr;
TextureHandle history1=nullptr;
ResourceSetHandle resolve0=nullptr;
ResourceSetHandle resolve1=nullptr;
ResourceSetHandle present0=nullptr;
ResourceSetHandle present1=nullptr;
float previousTime=0;float previousJitter[2]={};
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
                        TextureDesc currentColorDesc;currentColorDesc.width=imageWidth;currentColorDesc.height=imageHeight;
            currentColorDesc.format=Format::R16G16B16A16_FLOAT;currentColorDesc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            currentColor=imageResources->Keep(device.CreateTexture(currentColorDesc));
            TextureDesc velocityDesc;velocityDesc.width=imageWidth;velocityDesc.height=imageHeight;
            velocityDesc.format=Format::R16G16B16A16_FLOAT;velocityDesc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            velocity=imageResources->Keep(device.CreateTexture(velocityDesc));
            TextureDesc history0Desc;history0Desc.width=imageWidth;history0Desc.height=imageHeight;
            history0Desc.format=Format::R16G16B16A16_FLOAT;history0Desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            history0=imageResources->Keep(device.CreateTexture(history0Desc));
            TextureDesc history1Desc;history1Desc.width=imageWidth;history1Desc.height=imageHeight;
            history1Desc.format=Format::R16G16B16A16_FLOAT;history1Desc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            history1=imageResources->Keep(device.CreateTexture(history1Desc));

                        std::vector<ResourceBinding> resolve0Bindings;
            resolve0Bindings.push_back({0,0,nullptr,currentColor,0,0,{}});
            resolve0Bindings.push_back({1,0,nullptr,velocity,0,0,{}});
            resolve0Bindings.push_back({2,0,nullptr,history1,0,0,{}});
            resolve0=imageResources->Keep(device.CreateResourceSet({resolvePipeline,resolve0Bindings.data(),uint32_t(resolve0Bindings.size())}));
            std::vector<ResourceBinding> resolve1Bindings;
            resolve1Bindings.push_back({0,0,nullptr,currentColor,0,0,{}});
            resolve1Bindings.push_back({1,0,nullptr,velocity,0,0,{}});
            resolve1Bindings.push_back({2,0,nullptr,history0,0,0,{}});
            resolve1=imageResources->Keep(device.CreateResourceSet({resolvePipeline,resolve1Bindings.data(),uint32_t(resolve1Bindings.size())}));
            std::vector<ResourceBinding> present0Bindings;
            present0Bindings.push_back({0,0,nullptr,history0,0,0,{}});
            present0Bindings.push_back({1,0,nullptr,velocity,0,0,{}});
            present0=imageResources->Keep(device.CreateResourceSet({presentPipeline,present0Bindings.data(),uint32_t(present0Bindings.size())}));
            std::vector<ResourceBinding> present1Bindings;
            present1Bindings.push_back({0,0,nullptr,history1,0,0,{}});
            present1Bindings.push_back({1,0,nullptr,velocity,0,0,{}});
            present1=imageResources->Keep(device.CreateResourceSet({presentPipeline,present1Bindings.data(),uint32_t(present1Bindings.size())}));

        }
        params.sizeTime[0]=float(imageWidth);params.sizeTime[1]=float(imageHeight);
        params.sizeTime[2]=fixedTime?timeValue:float(frame)/60.0f;

        const bool historyValid=!initializeImages;
        params.extra[0]=historyValid?1.0f:0.0f;
        params.extra[3]=historyValid?previousTime:params.sizeTime[2];
        params.jitter[2]=historyValid?previousJitter[0]:0;
        params.jitter[3]=historyValid?previousJitter[1]:0;
        params.jitter[0]=Halton(frame%8+1,2)-0.5f;
        params.jitter[1]=Halton(frame%8+1,3)-0.5f;
        if(params.sizeTime[3]==1) params.jitter[0]=params.jitter[1]=0;
        auto historyOutput=(frame%2==0)?history0:history1;
        auto resolveSet=(frame%2==0)?resolve0:resolve1;
        auto presentSet=(frame%2==0)?present0:present1;

        ResourceScope frameResources(device);
        auto* commands=frameResources.Keep(device.AcquireCommandList());
                if(initializeImages) {
            { const ResourceBarrierDesc barrier{nullptr,currentColor,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=currentColor;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,currentColor,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
            { const ResourceBarrierDesc barrier{nullptr,velocity,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=velocity;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,velocity,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
            { const ResourceBarrierDesc barrier{nullptr,history0,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=history0;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,history0,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
            { const ResourceBarrierDesc barrier{nullptr,history1,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=history1;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,history1,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
        }

                // 1. Jittered current color/depth plus unjittered UV motion.
        {
            const ResourceBarrierDesc currentColorBarrier{nullptr,currentColor,ResourceState::ShaderResource,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&currentColorBarrier,1);
            const ResourceBarrierDesc velocityBarrier{nullptr,velocity,ResourceState::ShaderResource,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&velocityBarrier,1);
            std::array<ColorAttachment,2> colors{};
            colors[0].texture=currentColor;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            colors[1].texture=velocity;colors[1].loadOp=LoadOp::Clear;colors[1].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(scenePipeline);
            commands->SetViewport({0,0,float(imageWidth),float(imageHeight),0,1});commands->SetScissor({0,0,imageWidth,imageHeight});
            commands->SetInlineConstants(0,sizeof(params),&params);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
            const ResourceBarrierDesc currentColorReady{nullptr,currentColor,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&currentColorReady,1);
            const ResourceBarrierDesc velocityReady{nullptr,velocity,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&velocityReady,1);
        }
        // 2. Reproject previous history, reject disocclusion and clamp neighborhood.
        {
            const ResourceBarrierDesc historyOutputBarrier{nullptr,historyOutput,ResourceState::ShaderResource,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&historyOutputBarrier,1);
            std::array<ColorAttachment,1> colors{};
            colors[0].texture=historyOutput;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(resolvePipeline);
            commands->BindResourceSet(resolveSet);
            commands->SetViewport({0,0,float(imageWidth),float(imageHeight),0,1});commands->SetScissor({0,0,imageWidth,imageHeight});
            commands->SetInlineConstants(0,sizeof(params),&params);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
            const ResourceBarrierDesc historyOutputReady{nullptr,historyOutput,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&historyOutputReady,1);
        }
        // 3. Display resolved history; history ping-pongs across submitted frames.
        {
            const ResourceBarrierDesc backbufferBarrier{nullptr,backbuffer,ResourceState::Present,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&backbufferBarrier,1);
            std::array<ColorAttachment,1> colors{};
            colors[0].texture=backbuffer;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(presentPipeline);
            commands->BindResourceSet(presentSet);
            commands->SetViewport({0,0,float(imageWidth),float(imageHeight),0,1});commands->SetScissor({0,0,imageWidth,imageHeight});
            commands->SetInlineConstants(0,sizeof(params),&params);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
        }

        const ResourceBarrierDesc presentBarrier{nullptr,backbuffer,ResourceState::RenderTarget,ResourceState::Present,{}};
        commands->ResourceBarrier(&presentBarrier,1);
        if(!commands->Close() || !device.Submit(&commands,1)) throw std::runtime_error("Frame recording/submission failed.");
        // CPU state is committed only after Submit succeeds; GPU execution remains ordered on this queue.
        initializeImages=false;
        previousTime=params.sizeTime[2];previousJitter[0]=params.jitter[0];previousJitter[1]=params.jitter[1];
        ++frame;
        if(!capture.empty() && frame==frameLimit) Capture(device,backbuffer,capture);
        if(!device.Present()) throw std::runtime_error("Present failed.");
    }
    if(!device.WaitIdle()) throw std::runtime_error("Final GPU wait failed.");
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"TemporalAA: %s\n",error.what());return 1;
}
