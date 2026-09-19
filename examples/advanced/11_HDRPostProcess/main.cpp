#include "dyf.h"
#include "dyf/RHI.h"
#include "vertex.h"
#include "scene.h"
#include "bright.h"
#include "blur.h"
#include "tonemap.h"
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
    params.settings[1]=1;params.settings[2]=0.35f;params.extra[0]=12;
    uint64_t frameLimit=0;
    bool framesSpecified=false,validation=false,fixedTime=false;
    float timeValue=0;
    std::string capture;
    for(int i=1;i<argc;i++) {
        std::string arg=argv[i];
        if(arg=="--help") {
            std::puts("HDRPostProcess\n--mode aces|reinhard|clamp|no-bloom|bloom, --threshold [0,20], --bloom [0,5], --emission [1,100], --exposure [0.01,20]\n--frames N (0=unlimited), --capture path.ppm, --time SECONDS, --validation\n--capture defaults to 1 frames unless --frames is supplied.");
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
if(value=="aces") params.sizeTime[3]=0;
else if(value=="reinhard") params.sizeTime[3]=1;
else if(value=="clamp") params.sizeTime[3]=2;
else if(value=="no-bloom") params.sizeTime[3]=3;
else if(value=="bloom") params.sizeTime[3]=4;
else throw std::runtime_error("Unknown mode: "+value);
}else if(arg=="--threshold") { double n=Number(value,0,20);params.settings[1]=float(n);}
else if(arg=="--bloom") { double n=Number(value,0,5);params.settings[2]=float(n);}
else if(arg=="--emission") { double n=Number(value,1,100);params.extra[0]=float(n);}
else if(arg=="--exposure") { double n=Number(value,0.01,20);params.settings[0]=float(n);}

        else throw std::runtime_error("Unknown option: "+arg);
    }
    if(!capture.empty() && !framesSpecified) frameLimit=1;
    if(!capture.empty() && frameLimit==0) throw std::runtime_error("Capture requires a finite frame count.");
    dyf::Platform::Window window(960,640,"Advanced / HDRPostProcess");
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
auto brightShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::brightEntryPoint,ShaderData::bright,ShaderData::brightSize}));
auto blurShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::blurEntryPoint,ShaderData::blur,ShaderData::blurSize}));
auto tonemapShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::tonemapEntryPoint,ShaderData::tonemap,ShaderData::tonemapSize}));
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
    // Pipeline: bright
    std::vector<ResourceBindingLayout> brightBindings;
    brightBindings.push_back({0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    brightBindings.push_back({8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
    std::array<ColorAttachmentDesc,1> brightColors{};
    for(auto& attachment:brightColors) attachment={Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All};
    GraphicsPipelineDesc brightDesc;brightDesc.vertexShader=vertexShader;brightDesc.fragmentShader=brightShader;
    brightDesc.topology=PrimitiveTopology::TriangleList;brightDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    brightDesc.colorAttachments=brightColors.data();brightDesc.colorAttachmentCount=uint32_t(brightColors.size());
    brightDesc.layout={brightBindings.data(),uint32_t(brightBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto brightPipeline=resources.Keep(device.CreateGraphicsPipeline(brightDesc));
    // Pipeline: blur
    std::vector<ResourceBindingLayout> blurBindings;
    blurBindings.push_back({0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    blurBindings.push_back({8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
    std::array<ColorAttachmentDesc,1> blurColors{};
    for(auto& attachment:blurColors) attachment={Format::R16G16B16A16_FLOAT,{},ColorWriteMask::All};
    GraphicsPipelineDesc blurDesc;blurDesc.vertexShader=vertexShader;blurDesc.fragmentShader=blurShader;
    blurDesc.topology=PrimitiveTopology::TriangleList;blurDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    blurDesc.colorAttachments=blurColors.data();blurDesc.colorAttachmentCount=uint32_t(blurColors.size());
    blurDesc.layout={blurBindings.data(),uint32_t(blurBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto blurPipeline=resources.Keep(device.CreateGraphicsPipeline(blurDesc));
    // Pipeline: tonemap
    std::vector<ResourceBindingLayout> tonemapBindings;
    tonemapBindings.push_back({0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    tonemapBindings.push_back({1,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}});
    tonemapBindings.push_back({8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler});
    std::array<ColorAttachmentDesc,1> tonemapColors{};
    for(auto& attachment:tonemapColors) attachment={Format::B8G8R8A8_UNORM,{},ColorWriteMask::All};
    GraphicsPipelineDesc tonemapDesc;tonemapDesc.vertexShader=vertexShader;tonemapDesc.fragmentShader=tonemapShader;
    tonemapDesc.topology=PrimitiveTopology::TriangleList;tonemapDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    tonemapDesc.colorAttachments=tonemapColors.data();tonemapDesc.colorAttachmentCount=uint32_t(tonemapColors.size());
    tonemapDesc.layout={tonemapBindings.data(),uint32_t(tonemapBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto tonemapPipeline=resources.Keep(device.CreateGraphicsPipeline(tonemapDesc));


    std::unique_ptr<ResourceScope> imageResources;
    uint32_t imageWidth=0,imageHeight=0;
    bool initializeImages=true;
    TextureHandle hdrScene=nullptr;
TextureHandle bloomHorizontal=nullptr;
TextureHandle bloomVertical=nullptr;
ResourceSetHandle brightSet=nullptr;
ResourceSetHandle blurSet=nullptr;
ResourceSetHandle tonemapSet=nullptr;

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
                        TextureDesc hdrSceneDesc;hdrSceneDesc.width=imageWidth;hdrSceneDesc.height=imageHeight;
            hdrSceneDesc.format=Format::R16G16B16A16_FLOAT;hdrSceneDesc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            hdrScene=imageResources->Keep(device.CreateTexture(hdrSceneDesc));
            TextureDesc bloomHorizontalDesc;bloomHorizontalDesc.width=imageWidth;bloomHorizontalDesc.height=imageHeight;
            bloomHorizontalDesc.format=Format::R16G16B16A16_FLOAT;bloomHorizontalDesc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            bloomHorizontal=imageResources->Keep(device.CreateTexture(bloomHorizontalDesc));
            TextureDesc bloomVerticalDesc;bloomVerticalDesc.width=imageWidth;bloomVerticalDesc.height=imageHeight;
            bloomVerticalDesc.format=Format::R16G16B16A16_FLOAT;bloomVerticalDesc.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            bloomVertical=imageResources->Keep(device.CreateTexture(bloomVerticalDesc));

                        std::vector<ResourceBinding> brightSetBindings;
            brightSetBindings.push_back({0,0,nullptr,hdrScene,0,0,{}});
            brightSet=imageResources->Keep(device.CreateResourceSet({brightPipeline,brightSetBindings.data(),uint32_t(brightSetBindings.size())}));
            std::vector<ResourceBinding> blurSetBindings;
            blurSetBindings.push_back({0,0,nullptr,bloomHorizontal,0,0,{}});
            blurSet=imageResources->Keep(device.CreateResourceSet({blurPipeline,blurSetBindings.data(),uint32_t(blurSetBindings.size())}));
            std::vector<ResourceBinding> tonemapSetBindings;
            tonemapSetBindings.push_back({0,0,nullptr,hdrScene,0,0,{}});
            tonemapSetBindings.push_back({1,0,nullptr,bloomVertical,0,0,{}});
            tonemapSet=imageResources->Keep(device.CreateResourceSet({tonemapPipeline,tonemapSetBindings.data(),uint32_t(tonemapSetBindings.size())}));

        }
        params.sizeTime[0]=float(imageWidth);params.sizeTime[1]=float(imageHeight);
        params.sizeTime[2]=fixedTime?timeValue:float(frame)/60.0f;

        ResourceScope frameResources(device);
        auto* commands=frameResources.Keep(device.AcquireCommandList());
                if(initializeImages) {
            { const ResourceBarrierDesc barrier{nullptr,hdrScene,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=hdrScene;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,hdrScene,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
            { const ResourceBarrierDesc barrier{nullptr,bloomHorizontal,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=bloomHorizontal;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,bloomHorizontal,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
            { const ResourceBarrierDesc barrier{nullptr,bloomVertical,ResourceState::Undefined,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&barrier,1);
              ColorAttachment a;a.texture=bloomVertical;a.loadOp=LoadOp::Clear;a.storeOp=StoreOp::Store;commands->BeginRendering({&a,1,nullptr});
              commands->EndRendering();const ResourceBarrierDesc ready{nullptr,bloomVertical,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&ready,1); }
        }

                // 1. Linear HDR scene preserves radiance above 1.0.
        {
            const ResourceBarrierDesc hdrSceneBarrier{nullptr,hdrScene,ResourceState::ShaderResource,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&hdrSceneBarrier,1);
            std::array<ColorAttachment,1> colors{};
            colors[0].texture=hdrScene;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(scenePipeline);
            commands->SetViewport({0,0,float(imageWidth),float(imageHeight),0,1});commands->SetScissor({0,0,imageWidth,imageHeight});
            commands->SetInlineConstants(0,sizeof(params),&params);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
            const ResourceBarrierDesc hdrSceneReady{nullptr,hdrScene,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&hdrSceneReady,1);
        }
        // 2. Bright extraction and horizontal Gaussian convolution.
        {
            const ResourceBarrierDesc bloomHorizontalBarrier{nullptr,bloomHorizontal,ResourceState::ShaderResource,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&bloomHorizontalBarrier,1);
            std::array<ColorAttachment,1> colors{};
            colors[0].texture=bloomHorizontal;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(brightPipeline);
            commands->BindResourceSet(brightSet);
            commands->SetViewport({0,0,float(imageWidth),float(imageHeight),0,1});commands->SetScissor({0,0,imageWidth,imageHeight});
            commands->SetInlineConstants(0,sizeof(params),&params);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
            const ResourceBarrierDesc bloomHorizontalReady{nullptr,bloomHorizontal,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&bloomHorizontalReady,1);
        }
        // 3. Vertical Gaussian convolution.
        {
            const ResourceBarrierDesc bloomVerticalBarrier{nullptr,bloomVertical,ResourceState::ShaderResource,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&bloomVerticalBarrier,1);
            std::array<ColorAttachment,1> colors{};
            colors[0].texture=bloomVertical;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(blurPipeline);
            commands->BindResourceSet(blurSet);
            commands->SetViewport({0,0,float(imageWidth),float(imageHeight),0,1});commands->SetScissor({0,0,imageWidth,imageHeight});
            commands->SetInlineConstants(0,sizeof(params),&params);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
            const ResourceBarrierDesc bloomVerticalReady{nullptr,bloomVertical,ResourceState::RenderTarget,ResourceState::ShaderResource,{}};commands->ResourceBarrier(&bloomVerticalReady,1);
        }
        // 4. Bloom composition, exposure, tone mapping, display transfer.
        {
            const ResourceBarrierDesc backbufferBarrier{nullptr,backbuffer,ResourceState::Present,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&backbufferBarrier,1);
            std::array<ColorAttachment,1> colors{};
            colors[0].texture=backbuffer;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(tonemapPipeline);
            commands->BindResourceSet(tonemapSet);
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
    std::fprintf(stderr,"HDRPostProcess: %s\n",error.what());return 1;
}
