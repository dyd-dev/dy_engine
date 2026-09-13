#include "dyf/RHI.h"
#include "dyf/Platform/Window.h"
#include "vertex.h"
#include "fragment.h"
#include "producer.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
    bool validation=false, wait=false;
    std::string capture;
};
Options Parse(int argc,char** argv) {
    Options options;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        auto value=[&]() -> std::string {if(++i==argc)throw std::runtime_error("Missing option value");return argv[i];};
        if(arg=="--frames")options.frames=static_cast<uint32_t>(std::stoul(value()));
        else if(arg=="--flight")options.flight=static_cast<uint32_t>(std::stoul(value()));
        else if(arg=="--capture")options.capture=value();
        else if(arg=="--validation")options.validation=true;
        else if(arg=="--wait")options.wait=true;
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
        std::cout<<"ResourceBarriers: --wait submits and waits after each producer pass; default records all passes and submits once.\nCommon: --frames N --capture path.ppm --validation\n";return 0;
    }
    const auto options=Parse(argc,argv);
    dyf::Platform::Window window(800,600,"Advanced / ResourceBarriers");
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

    auto* producerShader=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::producerEntryPoint,ShaderData::producer,ShaderData::producerSize}));
    GraphicsPipelineDesc producerDesc=desc;producerDesc.fragmentShader=producerShader;
    ColorAttachmentDesc intermediateFormat{Format::R8G8B8A8_UNORM,{},ColorWriteMask::All};producerDesc.colorAttachments=&intermediateFormat;
    auto* producer=resources.Keep(device.CreateGraphicsPipeline(producerDesc));
    SamplerDesc sampler;sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
    sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
    sampler.mipLodBias=sampler.minLod=sampler.maxLod=0;
    const ResourceBindingLayout layout[]={{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
        {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}};
    desc.layout.bindings=layout;desc.layout.bindingCount=2;
    auto* display=resources.Keep(device.CreateGraphicsPipeline(desc));
    TextureHandle intermediate=nullptr;ResourceSetHandle bindings=nullptr;
    ResourceState intermediateState=ResourceState::Undefined;
    uint32_t frame=0;std::vector<double> cpuMs;
    while(!options.frames || frame<options.frames){
        window.PollEvents();if(!window.IsRunning())break;
        if(!device.BeginFrame()){Check(!device.IsLost(),"Device lost");std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
        auto* target=device.GetBackBuffer();const auto& extent=target->GetDesc();
        if(!intermediate || intermediate->GetDesc().width!=extent.width || intermediate->GetDesc().height!=extent.height){
            if(bindings)device.DestroyResourceSet(bindings);
            if(intermediate)device.DestroyTexture(intermediate);
            TextureDesc texture;texture.width=extent.width;texture.height=extent.height;
            texture.format=Format::R8G8B8A8_UNORM;texture.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource;
            intermediate=device.CreateTexture(texture);Check(intermediate!=nullptr,"Intermediate texture creation failed");
            ResourceBinding input;input.binding=0;input.texture=intermediate;
            bindings=device.CreateResourceSet({display,&input,1});Check(bindings!=nullptr,"ResourceSet creation failed");intermediateState=ResourceState::Undefined;
        }
        ResourceScope frameResources(device);auto* commands=frameResources.Keep(device.AcquireCommandList());
        const auto start=std::chrono::steady_clock::now();
        const ResourceBarrierDesc before{nullptr,intermediate,intermediateState,ResourceState::RenderTarget, {}};
        commands->ResourceBarrier(&before,1);
        float parameters[4]={0,0,0,0};
        // 각 패스는 직접 Begin/EndRendering을 기록한다. RHI가 후처리 순서를 결정하지 않는다.
        for(uint32_t pass=0;pass<2;++pass){
            if(pass){const ResourceBarrierDesc writeAfterWrite{nullptr,intermediate,ResourceState::RenderTarget,ResourceState::RenderTarget,{}};commands->ResourceBarrier(&writeAfterWrite,1);}
            ColorAttachment output;output.texture=intermediate;output.loadOp=pass?LoadOp::Load:LoadOp::Clear;output.storeOp=StoreOp::Store;output.clearColor[3]=1;
            commands->BeginRendering({&output,1,nullptr});
            commands->BindGraphicsPipeline(producer);commands->SetViewport({0,0,float(extent.width),float(extent.height),0,1});commands->SetScissor({0,0,extent.width,extent.height});
            parameters[0]=float(pass);commands->SetInlineConstants(0,sizeof(parameters),parameters);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
            if(options.wait){
                Check(commands->Close(),"Producer close failed");FenceHandle completion;
                Check(device.Submit({&commands,1,nullptr,0},completion),"Producer submit failed");Check(device.Wait(completion,UINT64_MAX),"Producer wait failed");
                commands=frameResources.Keep(device.AcquireCommandList());
            }
        }
        const ResourceBarrierDesc ready[]={{nullptr,intermediate,ResourceState::RenderTarget,ResourceState::ShaderResource,{}},
            {nullptr,target,ResourceState::Present,ResourceState::RenderTarget,{}}};commands->ResourceBarrier(ready,2);
        ColorAttachment color;color.texture=target;color.loadOp=LoadOp::Discard;color.storeOp=StoreOp::Store;
        commands->BeginRendering({&color,1,nullptr});commands->BindGraphicsPipeline(display);commands->BindResourceSet(bindings);
        commands->SetViewport({0,0,float(extent.width),float(extent.height),0,1});commands->SetScissor({0,0,extent.width,extent.height});
        parameters[0]=0;
        commands->SetInlineConstants(0,sizeof(parameters),parameters);commands->DrawInstanced(3,1,0,0);commands->EndRendering();
        ResourceBarrierDesc after{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};commands->ResourceBarrier(&after,1);
        Check(commands->Close(),"Display close failed");Check(device.Submit(&commands,1),"Display submit failed");intermediateState=ResourceState::ShaderResource;
        cpuMs.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
        if(cpuMs.size()>4096)cpuMs.erase(cpuMs.begin(),cpuMs.begin()+2048);
        if(!options.capture.empty() && frame+1==options.frames)SaveCapture(device,target,options.capture);
        Check(device.Present(),"Present failed");++frame;
    }
    Statistics("CPU record/submit incl optional waits",cpuMs);
    if(bindings)device.DestroyResourceSet(bindings);
    if(intermediate)device.DestroyTexture(intermediate);
    Check(device.WaitIdle(),"WaitIdle failed");
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
