#include "dyf.h"
#include "dyf/RHI.h"
#include "vertex.h"
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

    uint64_t frameLimit=0;
    bool framesSpecified=false,validation=false,fixedTime=false;
    float timeValue=0;
    std::string capture;
    for(int i=1;i<argc;i++) {
        std::string arg=argv[i];
        if(arg=="--help") {
            std::puts("PBRLighting\n--mode pbr|lambert|normal, --roughness [0.045,1], --metallic [0,1], --exposure [0.01,20]\n--frames N (0=unlimited), --capture path.ppm, --time SECONDS, --validation\n--capture defaults to 1 frames unless --frames is supplied.");
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
if(value=="pbr") params.sizeTime[3]=0;
else if(value=="lambert") params.sizeTime[3]=1;
else if(value=="normal") params.sizeTime[3]=2;
else throw std::runtime_error("Unknown mode: "+value);
}else if(arg=="--roughness") { double n=Number(value,0.045,1);params.settings[1]=float(n);}
else if(arg=="--metallic") { double n=Number(value,0,1);params.settings[2]=float(n);}
else if(arg=="--exposure") { double n=Number(value,0.01,20);params.settings[0]=float(n);}

        else throw std::runtime_error("Unknown option: "+arg);
    }
    if(!capture.empty() && !framesSpecified) frameLimit=1;
    if(!capture.empty() && frameLimit==0) throw std::runtime_error("Capture requires a finite frame count.");
    dyf::Platform::Window window(960,640,"Advanced / PBRLighting");
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
auto lightingShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::lightingEntryPoint,ShaderData::lighting,ShaderData::lightingSize}));
    // Pipeline: lighting
    std::vector<ResourceBindingLayout> lightingBindings;
    std::array<ColorAttachmentDesc,1> lightingColors{};
    for(auto& attachment:lightingColors) attachment={Format::B8G8R8A8_UNORM,{},ColorWriteMask::All};
    GraphicsPipelineDesc lightingDesc;lightingDesc.vertexShader=vertexShader;lightingDesc.fragmentShader=lightingShader;
    lightingDesc.topology=PrimitiveTopology::TriangleList;lightingDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    lightingDesc.colorAttachments=lightingColors.data();lightingDesc.colorAttachmentCount=uint32_t(lightingColors.size());
    lightingDesc.layout={lightingBindings.data(),uint32_t(lightingBindings.size()),sizeof(Params),ShaderStageFlags::Fragment,15};
    auto lightingPipeline=resources.Keep(device.CreateGraphicsPipeline(lightingDesc));


    uint32_t imageWidth=0,imageHeight=0;

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
        imageWidth=dimensions.width;imageHeight=dimensions.height;
        params.sizeTime[0]=float(imageWidth);params.sizeTime[1]=float(imageHeight);
        params.sizeTime[2]=fixedTime?timeValue:float(frame)/60.0f;

        ResourceScope frameResources(device);
        auto* commands=frameResources.Keep(device.AcquireCommandList());

                // PBR
        {
            const ResourceBarrierDesc backbufferBarrier{nullptr,backbuffer,ResourceState::Present,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&backbufferBarrier,1);
            std::array<ColorAttachment,1> colors{};
            colors[0].texture=backbuffer;colors[0].loadOp=LoadOp::Clear;colors[0].storeOp=StoreOp::Store;
            commands->BeginRendering({colors.data(),uint32_t(colors.size()),nullptr});
            commands->BindGraphicsPipeline(lightingPipeline);
            commands->SetViewport({0,0,float(imageWidth),float(imageHeight),0,1});commands->SetScissor({0,0,imageWidth,imageHeight});
            commands->SetInlineConstants(0,sizeof(params),&params);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
        }

        const ResourceBarrierDesc presentBarrier{nullptr,backbuffer,ResourceState::RenderTarget,ResourceState::Present,{}};
        commands->ResourceBarrier(&presentBarrier,1);
        if(!commands->Close() || !device.Submit(&commands,1)) throw std::runtime_error("Frame recording/submission failed.");
        // CPU state is committed only after Submit succeeds; GPU execution remains ordered on this queue.

        ++frame;
        if(!capture.empty() && frame==frameLimit) Capture(device,backbuffer,capture);
        if(!device.Present()) throw std::runtime_error("Present failed.");
    }
    if(!device.WaitIdle()) throw std::runtime_error("Final GPU wait failed.");
    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr,"PBRLighting: %s\n",error.what());return 1;
}
