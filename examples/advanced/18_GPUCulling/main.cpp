#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include "dyf/RHI/ResourceScope.h"
#include "cull.h"
#include "vertex.h"
#include "fragment.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace dyf::RHI;

int main(int argc,char** argv) try
{
    const auto check=[](bool ok,const char* message) {if(!ok) throw std::runtime_error(message);};
    uint32_t objectCount=1600,frameLimit=0;
    bool validation=false;
    for(int i=1;i<argc;++i)
    {
        const std::string_view argument=argv[i];
        if(argument=="--help")
        {
            std::puts("AdvancedGPUCulling [--objects N] [--frames N] [--validation]\n"
                "GPU frustum -> compact visible IDs -> DrawIndirect; no readback.\n"
                "U GPU indirect / CPU direct | C frustum on/off | Space pause | Escape close\n"
                "Objects: 1..4194240, default 1600. Frames: 0 = unlimited.\n"
                "Exit codes: success 0, error 1, unsupported device 77.");
            return 0;
        }
        if(argument=="--validation") validation=true;
        else if((argument=="--objects" || argument=="--frames") && i+1<argc)
        {
            const std::string_view value=argv[++i];uint32_t number=0;
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
            check(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size(),"Expected an unsigned integer.");
            if(argument=="--objects")
            {
                // 64개 스레드와 최대 65535개 작업 그룹으로 한 차원에서 처리한다.
                check(number>0 && number<=64u*65535u,"--objects must be in 1..4194240.");
                objectCount=number;
            }
            else frameLimit=number;
        }
        else throw std::runtime_error("Unknown or incomplete option; use --help.");
    }

    dyf::Platform::Window window(1100,720,"Advanced / GPU frustum culling - U GPU/CPU | C culling | Space pause");
    check(window.GetHandle()!=nullptr,"Window creation failed.");
    DeviceDesc deviceDesc;deviceDesc.enableValidation=validation;
    std::unique_ptr<IDevice> owner(IDevice::Create(deviceDesc));
    check(bool(owner),"Device creation failed.");
    auto& device=*owner;
    if(!device.Supports(Feature::Rasterization) || !device.Supports(Feature::Compute) ||
        !device.Supports(Feature::IndirectDraw))
    {
        std::puts("UNSUPPORTED: rasterization, compute and indirect drawing are required.");
        return 77;
    }
    ResourceScope resources(device);
    SwapchainDesc swapchain;swapchain.window=window.GetHandle();swapchain.format=Format::B8G8R8A8_UNORM;
    check(device.CreateSwapchain(swapchain),"Swapchain creation failed.");
    auto* cs=resources.Keep(device.CreateShader({ShaderStage::Compute,ShaderData::cullEntryPoint,ShaderData::cull,ShaderData::cullSize}));
    auto* vs=resources.Keep(device.CreateShader({ShaderStage::Vertex,ShaderData::vertexEntryPoint,ShaderData::vertex,ShaderData::vertexSize}));
    auto* fs=resources.Keep(device.CreateShader({ShaderStage::Fragment,ShaderData::fragmentEntryPoint,ShaderData::fragment,ShaderData::fragmentSize}));

    struct Bounds {float x,y,z,radius;};
    struct Frame {float camera[4],projection[4];uint32_t controls[4];};
    std::vector<Bounds> objects(objectCount);
    std::vector<uint32_t> identity(objectCount),cpuVisible;
    cpuVisible.reserve(objectCount);
    const uint32_t side=uint32_t(std::ceil(std::sqrt(double(objectCount))));
    for(uint32_t i=0;i<objectCount;++i)
    {
        objects[i]={(float(i%side)-float(side-1)*.5f)*1.2f,
            (float(i/side)-float(side-1)*.5f)*1.2f,0,.38f};
        identity[i]=i;
    }
    BufferDesc inputDesc{uint32_t(objects.size()*sizeof(Bounds)),sizeof(Bounds),
        BufferUsage::Storage,ResourceState::CopyDestination};
    BufferDesc visibleDesc{uint32_t(identity.size()*sizeof(uint32_t)),sizeof(uint32_t),
        BufferUsage::Storage,ResourceState::CopyDestination};
    BufferDesc argumentDesc{sizeof(DrawIndirectArguments),sizeof(uint32_t),BufferUsage::Storage|BufferUsage::Indirect};
    auto* inputs=resources.Keep(device.CreateBuffer(inputDesc));
    auto* visible=resources.Keep(device.CreateBuffer(visibleDesc));
    auto* arguments=resources.Keep(device.CreateBuffer(argumentDesc));

    const ResourceBindingLayout computeBindings[]={
        {0,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {1,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {2,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}}
    };
    ComputePipelineDesc computeDesc;computeDesc.computeShader=cs;
    computeDesc.layout={computeBindings,3,sizeof(Frame),ShaderStageFlags::Compute,15};
    computeDesc.threadGroupSize[0]=64;
    auto* compute=resources.Keep(device.CreateComputePipeline(computeDesc));
    const ResourceBinding computeResources[]={
        {0,0,inputs,nullptr,0,inputDesc.size,{}},{1,0,visible,nullptr,0,visibleDesc.size,{}},
        {2,0,arguments,nullptr,0,argumentDesc.size,{}}
    };
    auto* computeSet=resources.Keep(device.CreateResourceSet({compute,computeResources,3}));
    const ResourceBindingLayout graphicsBindings[]={
        {0,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Vertex,{}},
        {1,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Vertex,{}}
    };
    const ColorAttachmentDesc format{swapchain.format,{},ColorWriteMask::All};
    GraphicsPipelineDesc graphicsDesc;graphicsDesc.vertexShader=vs;graphicsDesc.fragmentShader=fs;
    graphicsDesc.topology=PrimitiveTopology::TriangleList;
    graphicsDesc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};
    graphicsDesc.colorAttachments=&format;graphicsDesc.colorAttachmentCount=1;
    graphicsDesc.layout={graphicsBindings,2,sizeof(Frame),ShaderStageFlags::Vertex,15};
    auto* graphics=resources.Keep(device.CreateGraphicsPipeline(graphicsDesc));
    const ResourceBinding graphicsResources[]={
        {0,0,inputs,nullptr,0,inputDesc.size,{}},{1,0,visible,nullptr,0,visibleDesc.size,{}}
    };
    auto* graphicsSet=resources.Keep(device.CreateResourceSet({graphics,graphicsResources,2}));

    std::puts("GPU frustum + compact IDs + DrawIndirect | U CPU direct | C frustum | Space pause\n"
        "GPU visible count: N/A (no readback). LOD and Hi-Z are outside this example.");
    uint32_t frame=0;
    bool gpu=true,culling=true,paused=false;
    float time=0,reportTime=0;
    auto previous=std::chrono::steady_clock::now();
    ResourceState argumentState=ResourceState::Undefined;
    while(window.IsRunning() && (!frameLimit || frame<frameLimit))
    {
        window.PollEvents();
        const auto& input=window.GetInput();
        if(!window.IsRunning() || input.WasPressed(dyf::Platform::Key::Escape)) break;
        if(input.WasPressed(dyf::Platform::Key::U)) gpu=!gpu;
        if(input.WasPressed(dyf::Platform::Key::C)) culling=!culling;
        if(input.WasPressed(dyf::Platform::Key::Space)) paused=!paused;
        const auto now=std::chrono::steady_clock::now();
        const float elapsed=std::chrono::duration<float>(now-previous).count();previous=now;
        if(window.IsMinimized()) {window.WaitEvents(.01);continue;}
        if(!paused) time+=elapsed;
        if(!device.BeginFrame())
        {
            check(!device.IsLost(),"Device lost during BeginFrame.");
            window.WaitEvents(.001);continue;
        }
        ResourceScope frameResources(device);
        auto* commands=frameResources.Keep(device.AcquireCommandList());
        if(frame==0)
        {
            check(device.UpdateBuffer(*commands,inputs,0,objects.data(),inputDesc.size),"Object upload failed.");
            check(device.UpdateBuffer(*commands,visible,0,identity.data(),visibleDesc.size),"Identity upload failed.");
            const ResourceBarrierDesc uploaded[]={
                {inputs,nullptr,ResourceState::CopyDestination,ResourceState::ShaderResource,{}},
                {visible,nullptr,ResourceState::CopyDestination,ResourceState::ShaderResource,{}}
            };
            commands->ResourceBarrier(uploaded,2);
        }
        auto* target=device.GetBackBuffer();
        check(target!=nullptr,"Backbuffer unavailable.");
        const auto& extent=target->GetDesc();
        Frame data={{8*std::sin(time*.23f),4*std::cos(time*.17f),-18,time},
            {1.7320508f*float(extent.height)/float(extent.width),1.7320508f,.1f,80},
            {objectCount,0,gpu?1u:0u,0}};
        if(gpu)
        {
            const ResourceBarrierDesc writable[]={
                {visible,nullptr,ResourceState::ShaderResource,ResourceState::UnorderedAccess,{}},
                {arguments,nullptr,argumentState,ResourceState::UnorderedAccess,{}}
            };
            commands->ResourceBarrier(writable,2);
            commands->BindComputePipeline(compute);commands->BindResourceSet(computeSet);
            commands->SetInlineConstants(0,sizeof(data),&data);commands->Dispatch(1,1,1);
            // 카운터 초기화를 모든 판정 스레드보다 먼저 끝낸다.
            const ResourceBarrierDesc reset{arguments,nullptr,ResourceState::UnorderedAccess,ResourceState::UnorderedAccess,{}};
            commands->ResourceBarrier(&reset,1);
            data.controls[1]=culling?1u:2u;
            commands->SetInlineConstants(0,sizeof(data),&data);commands->Dispatch((objectCount+63)/64,1,1);
            const ResourceBarrierDesc generated[]={
                {visible,nullptr,ResourceState::UnorderedAccess,ResourceState::ShaderResource,{}},
                {arguments,nullptr,ResourceState::UnorderedAccess,ResourceState::IndirectArgument,{}}
            };
            commands->ResourceBarrier(generated,2);
            argumentState=ResourceState::IndirectArgument;
        }
        else
        {
            // GPU와 같은 구·평면 판정을 사용하고 객체마다 일반 draw를 기록한다.
            cpuVisible.clear();
            for(uint32_t i=0;i<objectCount;++i)
            {
                const auto& bounds=objects[i];const float phase=.37f*i;
                const float x=bounds.x+.25f*std::sin(time+phase)-data.camera[0];
                const float y=bounds.y+.25f*std::cos(time*.8f+phase)-data.camera[1];
                const float z=bounds.z-data.camera[2],r=bounds.radius;
                const float sx=data.projection[0],sy=data.projection[1];
                if(!culling || (z+r>=data.projection[2] && z-r<=data.projection[3] &&
                    std::fabs(x)*sx<=z+r*std::sqrt(sx*sx+1) && std::fabs(y)*sy<=z+r*std::sqrt(sy*sy+1)))
                    cpuVisible.push_back(i);
            }
        }

        const ResourceBarrierDesc begin{nullptr,target,ResourceState::Present,ResourceState::RenderTarget,{}};
        commands->ResourceBarrier(&begin,1);
        ColorAttachment color;color.texture=target;color.loadOp=LoadOp::Clear;color.storeOp=StoreOp::Store;
        color.clearColor[0]=.018f;color.clearColor[1]=.025f;color.clearColor[2]=.045f;color.clearColor[3]=1;
        commands->BeginRendering({&color,1,nullptr});
        commands->BindGraphicsPipeline(graphics);commands->BindResourceSet(graphicsSet);
        commands->SetViewport({0,0,float(extent.width),float(extent.height),0,1});
        commands->SetScissor({0,0,extent.width,extent.height});
        if(gpu)
        {
            commands->SetInlineConstants(0,sizeof(data),&data);commands->DrawIndirect(arguments);
        }
        else for(const auto id:cpuVisible)
        {
            data.controls[3]=id;commands->SetInlineConstants(0,sizeof(data),&data);
            commands->DrawInstanced(3,1,0,0);
        }
        commands->EndRendering();
        const ResourceBarrierDesc end{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};
        commands->ResourceBarrier(&end,1);
        check(commands->Close(),"Command recording failed.");
        check(device.Submit(&commands,1),"Submission failed.");
        check(device.Present(),"Present failed.");
        ++frame;reportTime+=elapsed;
        if(reportTime>=1)
        {
            if(gpu) std::printf("GPU | inputs %u | visible N/A | indirect submissions 1 | frustum %d\n",objectCount,culling);
            else std::printf("CPU | inputs %u | visible %zu | direct submissions %zu | frustum %d\n",
                objectCount,cpuVisible.size(),cpuVisible.size(),culling);
            reportTime=0;
        }
    }
    check(device.WaitIdle(),"WaitIdle failed.");
    return 0;
}
catch(const std::exception& error)
{
    std::fprintf(stderr,"AdvancedGPUCulling: %s\n",error.what());return 1;
}
