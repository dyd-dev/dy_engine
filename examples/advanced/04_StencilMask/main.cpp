#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "vertex.h"
#include "fragment.h"
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
using namespace dyf::RHI;
namespace {
struct Options {
    uint32_t frames=0, reference=1;
    bool validation=false;
    std::string mode="mask", capture;
};
Options Parse(int argc,char** argv) {
    Options options;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        auto value=[&]() -> std::string {if(++i==argc)throw std::runtime_error("Missing option value");return argv[i];};
        if(arg=="--frames")options.frames=static_cast<uint32_t>(std::stoul(value()));
        else if(arg=="--reference")options.reference=static_cast<uint32_t>(std::stoul(value()));
        else if(arg=="--mode")options.mode=value();
        else if(arg=="--capture")options.capture=value();
        else if(arg=="--validation")options.validation=true;
        else throw std::runtime_error("Unknown option: "+arg);
    }
    if(options.reference>255)
        throw std::runtime_error("Invalid option range");
    if(!options.capture.empty() && !options.frames)options.frames=1;
    return options;
}
void Check(bool ok,const char* operation) {if(!ok)throw std::runtime_error(operation);}
void SaveCapture(IDevice& device,TextureHandle target,const std::string& path) {
    TextureReadback readback;
    Check(device.ReadTexture(target,readback),"ReadTexture failed");
    std::ofstream file(path,std::ios::binary);
    Check(static_cast<bool>(file),"Cannot open capture");
    file<<"P6\n"<<readback.width<<" "<<readback.height<<"\n255\n";
    const bool bgra=readback.format==Format::B8G8R8A8_UNORM || readback.format==Format::B8G8R8A8_UNORM_SRGB;
    for(uint32_t y=0;y<readback.height;++y)for(uint32_t x=0;x<readback.width;++x) {
        const auto* p=readback.pixels.data()+static_cast<size_t>(y)*readback.rowPitch+x*4;
        const char rgb[3]={static_cast<char>(p[bgra?2:0]),static_cast<char>(p[1]),static_cast<char>(p[bgra?0:2])};
        file.write(rgb,3);
    }
    Check(static_cast<bool>(file),"Capture write failed");
}
}
int main(int argc,char** argv) try {
    for(int i=1;i<argc;++i)if(std::string(argv[i])=="--help") {
        std::cout<<"StencilMask: --mode mask|outline --reference 0..255; circle is written with stencil reference 1.\nCommon: --frames N --capture path.ppm --validation\n";return 0;
    }
    const auto options=Parse(argc,argv);
    dyf::Platform::Window window(800,600,"Advanced / Stencil mask");
    Check(window.GetHandle()!=nullptr,"Window creation failed");
    DeviceDesc deviceDesc;deviceDesc.enableValidation=options.validation;
    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));
    Check(owner!=nullptr,"Device creation failed");auto& device=*owner;
    if(!device.Supports(Feature::Rasterization)){std::cout<<"UNSUPPORTED: this backend does not rasterize.\n";return 77;}
    ResourceScope resources(device);
    SwapchainDesc swapchain;swapchain.window=window.GetHandle();swapchain.format=Format::B8G8R8A8_UNORM;
    swapchain.minimumImageCount=2;swapchain.allowReadback=!options.capture.empty();

    Check(device.CreateSwapchain(swapchain),"Requested swapchain configuration unsupported");
    auto* vertex=resources.Keep(device.CreateShader({ShaderStage::Vertex,ShaderData::vertexEntryPoint,ShaderData::vertex,ShaderData::vertexSize}));
    auto* fragment=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::fragmentEntryPoint,ShaderData::fragment,ShaderData::fragmentSize}));
    ColorAttachmentDesc colorFormat{swapchain.format,{},ColorWriteMask::All};
    GraphicsPipelineDesc desc;desc.vertexShader=vertex;desc.fragmentShader=fragment;desc.topology=PrimitiveTopology::TriangleList;
    desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=&colorFormat;desc.colorAttachmentCount=1;
    desc.layout={nullptr,0,16,ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,15};
    Check(options.mode=="default" || options.mode=="mask" || options.mode=="outline","Invalid mode");
    auto& stencil=desc.depthStencil;stencil.format=Format::D24_UNORM_S8_UINT;stencil.stencilEnabled=true;
    stencil.stencilReadMask=255;stencil.stencilWriteMask=255;
    stencil.front=stencil.back={StencilOp::Keep,StencilOp::Keep,StencilOp::Replace,CompareOp::Always};
    colorFormat.writeMask=ColorWriteMask::None;
    if(!device.Supports(desc)){std::cout<<"UNSUPPORTED: stencil requires D24_UNORM_S8_UINT; current RHI has no D32S8/S8 alternative (including Apple Silicon).\n";return 77;}
    auto* stamp=resources.Keep(device.CreateGraphicsPipeline(desc));
    colorFormat.writeMask=ColorWriteMask::All;stencil.stencilWriteMask=0;
    stencil.front=stencil.back={StencilOp::Keep,StencilOp::Keep,StencilOp::Keep,options.mode=="outline"?CompareOp::NotEqual:CompareOp::Equal};
    auto* display=resources.Keep(device.CreateGraphicsPipeline(desc));
    TextureHandle depth=nullptr;ResourceState depthState=ResourceState::Undefined;uint32_t frame=0;
    while(!options.frames || frame<options.frames){
        window.PollEvents();if(!window.IsRunning())break;
        if(!device.BeginFrame()){Check(!device.IsLost(),"Device lost");std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
        auto* target=device.GetBackBuffer();const auto& extent=target->GetDesc();
        if(!depth || depth->GetDesc().width!=extent.width || depth->GetDesc().height!=extent.height){
            if(depth)device.DestroyTexture(depth);
            TextureDesc texture;texture.width=extent.width;texture.height=extent.height;texture.format=Format::D24_UNORM_S8_UINT;texture.usage=TextureUsage::DepthStencil;
            depth=device.CreateTexture(texture);Check(depth!=nullptr,"Depth/stencil texture creation failed");depthState=ResourceState::Undefined;
        }
        ResourceScope frameResources(device);auto* commands=frameResources.Keep(device.AcquireCommandList());
        ResourceBarrierDesc before[]={{nullptr,target,ResourceState::Present,ResourceState::RenderTarget,{}},{nullptr,depth,depthState,ResourceState::DepthWrite,{}}};commands->ResourceBarrier(before,2);
        ColorAttachment color;color.texture=target;color.loadOp=LoadOp::Clear;color.storeOp=StoreOp::Store;color.clearColor[0]=.04f;color.clearColor[1]=.05f;color.clearColor[2]=.08f;color.clearColor[3]=1;
        DepthStencilAttachment attachment;attachment.texture=depth;attachment.state=ResourceState::DepthWrite;
        attachment.depthLoadOp=attachment.stencilLoadOp=LoadOp::Clear;attachment.depthStoreOp=attachment.stencilStoreOp=StoreOp::Store;attachment.clearStencil=0;
        commands->BeginRendering({&color,1,&attachment});commands->SetViewport({0,0,float(extent.width),float(extent.height),0,1});commands->SetScissor({0,0,extent.width,extent.height});
        commands->BindGraphicsPipeline(stamp);commands->SetStencilReference(1);
        float settings[4]={0,float(extent.width)/float(extent.height),0,0};commands->SetInlineConstants(0,sizeof(settings),settings);commands->DrawInstanced(3,1,0,0);
        commands->BindGraphicsPipeline(display);commands->SetStencilReference(options.reference);
        settings[0]=1;settings[2]=options.mode=="outline"?1.f:0.f;commands->SetInlineConstants(0,sizeof(settings),settings);commands->DrawInstanced(3,1,0,0);
        commands->EndRendering();ResourceBarrierDesc end{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};commands->ResourceBarrier(&end,1);
        Check(commands->Close(),"Close failed");Check(device.Submit(&commands,1),"Submit failed");depthState=ResourceState::DepthWrite;
        if(!options.capture.empty() && frame+1==options.frames)SaveCapture(device,target,options.capture);
        Check(device.Present(),"Present failed");++frame;
    }
    if(depth)device.DestroyTexture(depth);
    Check(device.WaitIdle(),"WaitIdle failed");
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
