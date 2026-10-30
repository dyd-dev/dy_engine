#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include <cstdio>
#include <string>
#include <utility>

#include <fstream>
#include <iterator>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace dyf::RHI;

int main()
{
    // 예제 폴더에서 실행한다. 컴파일러가 포함된 엔진은 GLSL을 컴파일하고, 그 외에는 .bin을 읽는다.
    const auto loadShader = [](dyf::RHI::ShaderStage stage, const char* sourcePath, const char* binaryPath)
    {
        bool compile = dyf::RHI::IsShaderCompilerAvailable();
        const char* path = compile ? sourcePath : binaryPath;
        std::ifstream file(path, std::ios::binary);
        if(!file && compile)
        {
            compile = false;
            path = binaryPath;
            file.open(path, std::ios::binary);
        }
        if(!file) { std::fprintf(stderr, "Cannot open shader: %s\n", path); return std::vector<uint8_t>{}; }
        const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if(!compile) return std::vector<uint8_t>(data.begin(), data.end());
        auto result = dyf::RHI::CompileShader({stage, data, "main", sourcePath});
        if(!result.diagnostics.empty()) std::fprintf(stderr, "%s\n", result.diagnostics.c_str());
        return std::move(result.binary);
    };

    const auto cullBinary = loadShader(dyf::RHI::ShaderStage::Compute, "Shaders/cull.glsl", "Shaders/cull.bin");
#ifdef DY_EXAMPLE_METAL
    const char* cullEntry="cullObjects";
#else
    const char* cullEntry="main";
#endif
    const auto vertexBinary = loadShader(dyf::RHI::ShaderStage::Vertex, "Shaders/vertex.glsl", "Shaders/vertex.bin");
#ifdef DY_EXAMPLE_METAL
    const char* vertexEntry="vertexMain";
#else
    const char* vertexEntry="main";
#endif
    const auto fragmentBinary = loadShader(dyf::RHI::ShaderStage::Fragment, "Shaders/fragment.glsl", "Shaders/fragment.bin");
#ifdef DY_EXAMPLE_METAL
    const char* fragmentEntry="fragmentMain";
#else
    const char* fragmentEntry="main";
#endif
    if(cullBinary.empty() || vertexBinary.empty() || fragmentBinary.empty()) return 1;

    constexpr uint32_t objectCount=1600;
    dyf::Platform::Window window(1100,720,"Advanced / GPU frustum culling");
    std::unique_ptr<IDevice> owner(IDevice::Create({}));
    auto& device=*owner;
    ResourceScope resources(device);
    SwapchainDesc swapchain;
    swapchain.window=window.GetHandle();
    swapchain.format=DefaultSwapchainFormat;
    (void)device.CreateSwapchain(swapchain);
    auto* cs=resources.Keep(device.CreateShader({ShaderStage::Compute,cullEntry,cullBinary.data(),cullBinary.size()}));
    auto* vs=resources.Keep(device.CreateShader({ShaderStage::Vertex,vertexEntry,vertexBinary.data(),vertexBinary.size()}));
    auto* fs=resources.Keep(device.CreateShader({ShaderStage::Fragment,fragmentEntry,fragmentBinary.data(),fragmentBinary.size()}));

    struct Bounds {float x,y,z,radius;};
    struct Frame {float camera[4],projection[4];uint32_t controls[4];};
    std::vector<Bounds> objects(objectCount);
    constexpr uint32_t side=40;
    for(uint32_t i=0;i<objectCount;++i)
    {
        objects[i]={(float(i%side)-float(side-1)*.5f)*1.2f,
            (float(i/side)-float(side-1)*.5f)*1.2f,0,.38f};
    }
    BufferDesc inputDesc{uint32_t(objects.size()*sizeof(Bounds)),sizeof(Bounds),
        BufferUsage::Storage,ResourceState::CopyDestination};
    BufferDesc visibleDesc{objectCount*sizeof(uint32_t),sizeof(uint32_t),BufferUsage::Storage};
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

    const auto start=std::chrono::steady_clock::now();
    bool uploaded=false;
    while(window.IsRunning())
    {
        window.PollEvents();
        if(!window.IsRunning()) break;
        if(!device.BeginFrame()) continue;
        ResourceScope frameResources(device);
        auto* commands=frameResources.Keep(device.AcquireCommandList());
        if(!uploaded)
        {
            (void)device.UpdateBuffer(*commands,inputs,0,objects.data(),inputDesc.size);
            const ResourceBarrierDesc ready{inputs,nullptr,
                ResourceState::CopyDestination,ResourceState::ShaderResource,{}};
            commands->ResourceBarrier(&ready,1);
        }
        auto* target=device.GetBackBuffer();
        const auto extent=target->GetDesc();
        const float time=std::chrono::duration<float>(std::chrono::steady_clock::now()-start).count();
        Frame data={{8*std::sin(time*.23f),4*std::cos(time*.17f),-18,time},
            {1.7320508f*float(extent.height)/float(extent.width),1.7320508f,.1f,80},
            {objectCount,0,0,0}};

        const ResourceBarrierDesc writable[]={
            {visible,nullptr,uploaded?ResourceState::ShaderResource:ResourceState::Undefined,
                ResourceState::UnorderedAccess,{}},
            {arguments,nullptr,uploaded?ResourceState::IndirectArgument:ResourceState::Undefined,
                ResourceState::UnorderedAccess,{}}
        };
        commands->ResourceBarrier(writable,2);
        commands->BindComputePipeline(compute);
        commands->BindResourceSet(computeSet);
        // 첫 디스패치에서 간접 인수를 초기화한다.
        commands->SetInlineConstants(0,sizeof(data),&data);
        commands->Dispatch(1,1,1);
        const ResourceBarrierDesc reset{arguments,nullptr,
            ResourceState::UnorderedAccess,ResourceState::UnorderedAccess,{}};
        commands->ResourceBarrier(&reset,1);
        // 다음 디스패치에서 구와 절두체를 판정하고 보이는 ID를 압축한다.
        data.controls[1]=1;
        commands->SetInlineConstants(0,sizeof(data),&data);
        commands->Dispatch((objectCount+63)/64,1,1);
        const ResourceBarrierDesc generated[]={
            {visible,nullptr,ResourceState::UnorderedAccess,ResourceState::ShaderResource,{}},
            {arguments,nullptr,ResourceState::UnorderedAccess,ResourceState::IndirectArgument,{}}
        };
        commands->ResourceBarrier(generated,2);
        uploaded=true;

        const ResourceBarrierDesc begin{nullptr,target,
            ResourceState::Present,ResourceState::RenderTarget,{}};
        commands->ResourceBarrier(&begin,1);
        ColorAttachment color;
        color.texture=target;
        color.loadOp=LoadOp::Clear;
        color.storeOp=StoreOp::Store;
        color.clearColor[0]=.018f;
        color.clearColor[1]=.025f;
        color.clearColor[2]=.045f;
        color.clearColor[3]=1;
        commands->BeginRendering({&color,1,nullptr});
        commands->BindGraphicsPipeline(graphics);
        commands->BindResourceSet(graphicsSet);
        commands->SetViewport({0,0,float(extent.width),float(extent.height),0,1});
        commands->SetScissor({0,0,extent.width,extent.height});
        commands->SetInlineConstants(0,sizeof(data),&data);
        commands->DrawIndirect(arguments);
        commands->EndRendering();
        const ResourceBarrierDesc end{nullptr,target,
            ResourceState::RenderTarget,ResourceState::Present,{}};
        commands->ResourceBarrier(&end,1);
        (void)commands->Close();
        (void)device.Submit(&commands,1);
        (void)device.Present();
    }
    (void)device.WaitIdle();
}
