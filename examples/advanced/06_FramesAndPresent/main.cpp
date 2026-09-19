#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "vertex.h"
#include "fragment.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace dyf::RHI;
namespace {
struct Options {
    uint32_t frames=0, flight=2;
    bool validation=false;
    std::string mode="fifo", capture;
};
Options Parse(int argc,char** argv) {
    Options options;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        auto value=[&]() -> std::string {if(++i==argc)throw std::runtime_error("Missing option value");return argv[i];};
        if(arg=="--frames")options.frames=static_cast<uint32_t>(std::stoul(value()));
        else if(arg=="--flight")options.flight=static_cast<uint32_t>(std::stoul(value()));
        else if(arg=="--mode")options.mode=value();
        else if(arg=="--capture")options.capture=value();
        else if(arg=="--validation")options.validation=true;
        else throw std::runtime_error("Unknown option: "+arg);
    }
    if(options.flight<1 || options.flight>4)
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
void Statistics(const char* name,std::vector<double> samples) {
    if(samples.empty())return;
    std::sort(samples.begin(),samples.end());double sum=0;for(double sample:samples)sum+=sample;
    std::cout<<name<<": samples="<<samples.size()<<" mean="<<sum/samples.size()
        <<" ms median="<<samples[samples.size()/2]<<" ms p95="<<samples[(samples.size()-1)*95/100]<<" ms\n";
}
}
int main(int argc,char** argv) try {
    for(int i=1;i<argc;++i)if(std::string(argv[i])=="--help") {
        std::cout<<"FramesAndPresent: --flight 1..4 --mode fifo|immediate|mailbox; CPU durations do not measure end-to-end display latency.\nCommon: --frames N --capture path.ppm --validation\n";return 0;
    }
    const auto options=Parse(argc,argv);
    dyf::Platform::Window window(800,600,"Advanced / Frames and Present");
    Check(window.GetHandle()!=nullptr,"Window creation failed");
    DeviceDesc deviceDesc;deviceDesc.enableValidation=options.validation;deviceDesc.maxFramesInFlight=options.flight;
    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));
    Check(owner!=nullptr,"Device creation failed");auto& device=*owner;
    if(!device.Supports(Feature::Rasterization)){std::cout<<"UNSUPPORTED: this backend does not rasterize.\n";return 77;}
    ResourceScope resources(device);
    SwapchainDesc swapchain;swapchain.window=window.GetHandle();swapchain.format=Format::B8G8R8A8_UNORM;
    swapchain.minimumImageCount=2;swapchain.allowReadback=!options.capture.empty();
    if(options.mode=="default" || options.mode=="fifo")swapchain.presentMode=PresentMode::Fifo;
    else if(options.mode=="immediate")swapchain.presentMode=PresentMode::Immediate;
    else if(options.mode=="mailbox")swapchain.presentMode=PresentMode::Mailbox;
    else throw std::runtime_error("Invalid present mode");
    Check(device.CreateSwapchain(swapchain),"Requested swapchain configuration unsupported");
    auto* vertex=resources.Keep(device.CreateShader({ShaderStage::Vertex,ShaderData::vertexEntryPoint,ShaderData::vertex,ShaderData::vertexSize}));
    auto* fragment=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::fragmentEntryPoint,ShaderData::fragment,ShaderData::fragmentSize}));
    ColorAttachmentDesc colorFormat{swapchain.format,{},ColorWriteMask::All};
    GraphicsPipelineDesc desc;desc.vertexShader=vertex;desc.fragmentShader=fragment;desc.topology=PrimitiveTopology::TriangleList;
    desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    desc.colorAttachments=&colorFormat;desc.colorAttachmentCount=1;
    desc.layout={nullptr,0,16,ShaderStageFlags::Vertex|ShaderStageFlags::Fragment,15};
    auto* pipeline=resources.Keep(device.CreateGraphicsPipeline(desc));
    std::vector<ICommandList*> lists(options.flight);std::vector<FenceHandle> fences(options.flight);
    for(auto& list:lists)list=resources.Keep(device.AcquireCommandList());
    std::vector<double> waitMs,presentMs,frameMs;
    const auto start=std::chrono::steady_clock::now();uint32_t frame=0;
    auto previous=start;
    auto waitStart=start;bool acquisitionPending=false;
    while(!options.frames || frame<options.frames){
        window.PollEvents();if(!window.IsRunning())break;
        const uint32_t slot=frame%options.flight;
        if(!acquisitionPending){waitStart=std::chrono::steady_clock::now();acquisitionPending=true;}
        if(fences[slot])Check(device.Wait(fences[slot],UINT64_MAX),"Slot fence wait failed");
        Check(device.ResetCommandList(lists[slot]),"Reset in-flight commands rejected");
        if(!device.BeginFrame()){Check(!device.IsLost(),"Device lost");std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
        waitMs.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-waitStart).count());
        acquisitionPending=false;
        auto* target=device.GetBackBuffer();auto* commands=lists[slot];
        ResourceBarrierDesc before{nullptr,target,ResourceState::Present,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&before,1);
        ColorAttachment color;color.texture=target;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
        commands->BeginRendering({&color,1,nullptr});commands->BindGraphicsPipeline(pipeline);
        commands->SetViewport({0,0,float(target->GetDesc().width),float(target->GetDesc().height),0,1});commands->SetScissor({0,0,target->GetDesc().width,target->GetDesc().height});
        const float time=std::chrono::duration<float>(std::chrono::steady_clock::now()-start).count();
        const float settings[4]={time,0,0,0};commands->SetInlineConstants(0,sizeof(settings),settings);commands->DrawInstanced(3,1,0,0);
        commands->EndRendering();ResourceBarrierDesc after{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};commands->ResourceBarrier(&after,1);
        Check(commands->Close(),"Close failed");Check(device.Submit({&commands,1,nullptr,0},fences[slot]),"Submit failed");
        if(!options.capture.empty() && frame+1==options.frames)SaveCapture(device,target,options.capture);
        const auto presentStart=std::chrono::steady_clock::now();Check(device.Present(),"Present failed");
        const auto now=std::chrono::steady_clock::now();presentMs.push_back(std::chrono::duration<double,std::milli>(now-presentStart).count());
        frameMs.push_back(std::chrono::duration<double,std::milli>(now-previous).count());previous=now;++frame;
        if(frame%120==0){Statistics("CPU slot wait + acquisition",waitMs);Statistics("CPU Present call",presentMs);Statistics("CPU frame interval",frameMs);}
        for(auto* samples:{&waitMs,&presentMs,&frameMs})if(samples->size()>4096)samples->erase(samples->begin(),samples->begin()+2048);
    }
    Statistics("CPU slot wait + acquisition",waitMs);Statistics("CPU Present call",presentMs);Statistics("CPU frame interval",frameMs);
    Check(device.WaitIdle(),"WaitIdle failed");
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
