#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "dyf/Platform/Profiler.h"
#include "dyf/Platform/RenderDocCapture.h"
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
    uint32_t frames=0, flight=2, work=96;
    bool validation=false;
    std::string capture;
};
Options Parse(int argc,char** argv) {
    Options options;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        auto value=[&]() -> std::string {if(++i==argc)throw std::runtime_error("Missing option value");return argv[i];};
        if(arg=="--frames")options.frames=static_cast<uint32_t>(std::stoul(value()));
        else if(arg=="--flight")options.flight=static_cast<uint32_t>(std::stoul(value()));
        else if(arg=="--work")options.work=static_cast<uint32_t>(std::stoul(value()));
        else if(arg=="--capture")options.capture=value();
        else if(arg=="--validation")options.validation=true;
        else throw std::runtime_error("Unknown option: "+arg);
    }
    if(options.flight<1 || options.flight>4 || options.work>2048)
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
        std::cout<<"Profiling: --work 0..2048 --flight 1..4; GPU timestamp is optional, CPU timings always measured.\nCommon: --frames N --capture path.ppm --validation\nF12 requests RenderDoc capture when the API is injected; PIX/RenderDoc show the Profiling pass event.\n";return 0;
    }
    const auto options=Parse(argc,argv);
    dyf::Platform::Window window(800,600,"Advanced / Profiling");
    Check(window.GetHandle()!=nullptr,"Window creation failed");
    DeviceDesc deviceDesc;deviceDesc.enableValidation=options.validation;deviceDesc.maxFramesInFlight=options.flight;
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
    auto* pipeline=resources.Keep(device.CreateGraphicsPipeline(desc));
    const bool gpuTiming=device.Supports(Feature::TimestampQuery);
    std::cout<<"GPU timestamp: "<<(gpuTiming?"available":"UNSUPPORTED; CPU timing only")<<'\n';
    auto* query=gpuTiming?resources.Keep(device.CreateTimestampQuery({options.flight*2})):nullptr;
    std::vector<ICommandList*> lists(options.flight);std::vector<FenceHandle> fences(options.flight);
    for(auto& list:lists)list=resources.Keep(device.AcquireCommandList());
    std::vector<double> recordMs,submitMs,frameMs,gpuMs;
    const auto milliseconds=[](auto begin,auto end){return std::chrono::duration<double,std::milli>(end-begin).count();};
    auto readQuery=[&](uint32_t slot){
        if(!query || !fences[slot])return;
        uint64_t ticks[2];Check(device.ReadTimestamps(query,slot*2,2,ticks),"Completed timestamp query unavailable");
        const uint32_t bits=device.GetTimestampValidBits();
        const uint64_t mask=bits>=64?UINT64_MAX:((uint64_t{1}<<bits)-1);
        gpuMs.push_back(static_cast<double>((ticks[1]-ticks[0])&mask)*device.GetTimestampPeriodNanoseconds()/1e6);
    };
    uint32_t frame=0;
    auto frameStart=std::chrono::steady_clock::now();
    while(!options.frames || frame<options.frames){
        window.PollEvents();if(!window.IsRunning())break;
        const uint32_t slot=frame%options.flight;
        if(fences[slot]){Check(device.Wait(fences[slot],UINT64_MAX),"Fence wait failed");readQuery(slot);fences[slot]={};}
        Check(device.ResetCommandList(lists[slot]),"ResetCommandList failed");
        if(!device.BeginFrame()){Check(!device.IsLost(),"Device lost");std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
        auto* target=device.GetBackBuffer();auto* commands=lists[slot];
        const auto recordStart=std::chrono::steady_clock::now();
        DY_PROFILE_CPU_ZONE_NAMED("AdvancedProfiling::Record");
        commands->BeginDebugEvent("Profiling pass");
        commands->InsertDebugMarker("Timestamp begin");
        if(query){commands->ResetTimestamps(query,slot*2,2);commands->WriteTimestamp(query,slot*2);}
        ResourceBarrierDesc begin{nullptr,target,ResourceState::Present,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&begin,1);
        ColorAttachment color;color.texture=target;color.loadOp=LoadOp::Clear;color.storeOp=StoreOp::Store;color.clearColor[3]=1;
        commands->BeginRendering({&color,1,nullptr});commands->BindGraphicsPipeline(pipeline);
        commands->SetViewport({0,0,float(target->GetDesc().width),float(target->GetDesc().height),0,1});
        commands->SetScissor({0,0,target->GetDesc().width,target->GetDesc().height});
        const float parameters[4]={float(options.work),0,0,0};commands->SetInlineConstants(0,sizeof(parameters),parameters);
        commands->DrawInstanced(3,1,0,0);commands->EndRendering();
        ResourceBarrierDesc end{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};commands->ResourceBarrier(&end,1);
        if(query)commands->WriteTimestamp(query,slot*2+1);
        commands->EndDebugEvent();
        Check(commands->Close(),"Close failed");const auto submitStart=std::chrono::steady_clock::now();
        Check(device.Submit({&commands,1,nullptr,0},fences[slot]),"Submit failed");
        const auto submitEnd=std::chrono::steady_clock::now();
        if(!options.capture.empty() && frame+1==options.frames)SaveCapture(device,target,options.capture);
        Check(device.Present(),"Present failed");
        DY_PROFILE_FRAME_MARK();
        recordMs.push_back(milliseconds(recordStart,submitStart));submitMs.push_back(milliseconds(submitStart,submitEnd));
        const auto frameEnd=std::chrono::steady_clock::now();
        frameMs.push_back(milliseconds(frameStart,frameEnd));frameStart=frameEnd;++frame;
        if(frame%120==0){Statistics("CPU record",recordMs);Statistics("CPU Submit (native replay included)",submitMs);Statistics("GPU pass",gpuMs);}
        // 긴 실행에서 표본 저장이 무한히 증가하지 않게 최근 4096개를 보유한다.
        for(auto* samples:{&recordMs,&submitMs,&frameMs,&gpuMs})if(samples->size()>4096)samples->erase(samples->begin(),samples->begin()+2048);
    }
    for(uint32_t slot=0;slot<options.flight;++slot)if(fences[slot]){Check(device.Wait(fences[slot],UINT64_MAX),"Final wait failed");readQuery(slot);}
    std::cout<<"Cold frames included; capture/validation can affect measurement. GPU pass excludes Present.\n";
    Statistics("CPU record",recordMs);Statistics("CPU Submit",submitMs);Statistics("CPU frame incl waits/present",frameMs);Statistics("GPU pass",gpuMs);
    Check(device.WaitIdle(),"WaitIdle failed");
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
