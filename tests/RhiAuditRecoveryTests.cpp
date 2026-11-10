#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "Backends/Null/NullDevice.h"
#include "dyf/RHI.h"

namespace {
thread_local long failAfter=-1;
thread_local bool countAllocations=false;
thread_local size_t allocations=0;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
}
void* operator new(size_t size)
{
    if(countAllocations)++allocations;
    if(failAfter>=0 && failAfter--==0) {failAfter=-1;throw std::bad_alloc{};}
    if(void* memory=std::malloc(size?size:1))return memory;
    throw std::bad_alloc{};
}
void* operator new[](size_t size) {return ::operator new(size);}
void operator delete(void* memory) noexcept {std::free(memory);}
void operator delete[](void* memory) noexcept {std::free(memory);}
void operator delete(void* memory,size_t) noexcept {std::free(memory);}
void operator delete[](void* memory,size_t) noexcept {std::free(memory);}

namespace {
namespace R=dyf::RHI;
struct Device final:dyf::Backends::NullDevice
{
    uint64_t alignment;
    explicit Device(uint64_t value=1):alignment(value) {Check(Initialize(nullptr,{})==0,"Null initialization failed");}
    uint64_t GetLimitNative(R::Limit limit) const override
    {return limit==R::Limit::BufferUpdateAlignment?alignment:NullDevice::GetLimitNative(limit);}
    void Release() {ReleaseResources();}
    void Begin()
    {
        R::SwapchainDesc swap;swap.format=R::Format::R8G8B8A8_UNORM;swap.initialWidth=swap.initialHeight=16;
        Check(CreateSwapchain(swap) && BeginFrame(),"Null frame initialization failed");
    }
};
bool SubmitImage(Device& device,R::TextureHandle image)
{
    R::ResourceScope own(device);
    auto* commands=own.Keep(device.AcquireCommandList());
    Check(commands!=nullptr,"Command acquisition failed");
    const R::ResourceBarrierDesc barrier={nullptr,image,R::ResourceState::Present,R::ResourceState::Present,{}};
    commands->ResourceBarrier(&barrier,1);
    return commands->Close() && device.Submit(&commands,1);
}
void BackBufferRecovery()
{
    unsigned failures=0;
    for(long position=0;position<20;++position)
    {
        Device device;device.Begin();
        bool threw=false;failAfter=position;
        try {(void)device.GetBackBuffer();}catch(const std::bad_alloc&) {threw=true;}
        failAfter=-1;
        auto* image=device.GetBackBuffer();
        Check(image && SubmitImage(device,image) && device.Present(),
            "Failed backbuffer registration left an unusable handle after retry");
        if(!threw)break;
        ++failures;
    }
    Check(failures>0,"Backbuffer allocation faults were not exercised");
    std::printf("PASS backbuffer registration recovers from %u allocation failures\n",failures);
}
void ReleaseWithoutAllocation()
{
    Device device;
    auto* buffer=device.CreateBuffer({16,0,R::BufferUsage::Storage,R::ResourceState::Common});
    auto* commands=device.AcquireCommandList();
    Check(buffer && commands,"Release fixture creation failed");
    const R::ResourceBarrierDesc barrier={buffer,nullptr,R::ResourceState::Common,R::ResourceState::Common,{}};
    commands->ResourceBarrier(&barrier,1);Check(commands->Close(),"Release fixture close failed");
    allocations=0;countAllocations=true;
    device.Release();
    countAllocations=false;
    const auto observed=allocations;
    Check(device.GetResourceAllocationCounters().buffers.live==0,"Release retained a resource");
    Check(observed==0,"Device resource release must not allocate temporary reference storage");
    allocations=0;countAllocations=true;device.Release();countAllocations=false;
    Check(allocations==0,"Repeated empty release allocated");
    std::puts("PASS resource release is allocation-free and idempotent");
}
void GraphImportRecovery()
{
    Device device;R::ResourceScope own(device);
    auto* buffer=own.Keep(device.CreateBuffer({16,0,R::BufferUsage::Storage,R::ResourceState::Common}));
    Check(buffer!=nullptr,"Graph fixture buffer creation failed");
    const std::string name="first",alias="alias";
    unsigned failures=0;
    for(long position=0;position<32;++position)
    {
        R::RenderGraph graph;Check(graph.Compile(),"Empty graph did not compile");
        bool threw=false;failAfter=position;
        try {(void)graph.ImportBuffer(name,buffer,R::ResourceState::Common,R::ResourceState::CopyDestination);}
        catch(const std::bad_alloc&) {threw=true;}
        failAfter=-1;
        const auto first=graph.ImportBuffer(name,buffer,R::ResourceState::Common,R::ResourceState::CopyDestination);
        const auto second=graph.ImportBuffer(alias,buffer,R::ResourceState::Common,R::ResourceState::CopyDestination);
        Check(first.IsValid() && first==second,"Failed graph import split aliases of one physical resource");
        Check(!graph.IsCompiled() && graph.Compile(),"Successful import retained a stale compiled plan");
        if(!threw)break;
        ++failures;
    }
    Check(failures>0,"Graph allocation faults were not exercised");
    std::printf("PASS graph import preserves alias and plan consistency after %u allocation failures\n",failures);
}
void GraphWorkspaceReuse()
{
    Device device;device.Begin();R::ResourceScope own(device);
    auto* first=own.Keep(device.CreateBuffer({16,0,R::BufferUsage::Storage,R::ResourceState::Common}));
    auto* second=own.Keep(device.CreateBuffer({16,0,R::BufferUsage::Storage,R::ResourceState::Common}));
    Check(first && second,"Graph reuse fixture creation failed");
    R::RenderGraph graph;
    unsigned callbacks=0;
    const auto build=[&](R::BufferHandle buffer) {
        const auto resource=graph.ImportBuffer("buffer",buffer,R::ResourceState::Common,R::ResourceState::Common);
        Check(resource.IsValid(),"Graph reuse import failed");
        graph.AddPass("write").Write(resource,R::ResourceState::UnorderedAccess).SetExecute([&](auto*) {++callbacks;});
        graph.AddPass("read").Read(resource,R::ResourceState::UnorderedAccess).SetExecute([&](auto*) {++callbacks;});
        Check(graph.Compile(),"Graph reuse compile failed");
        return resource;
    };
    const auto oldHandle=build(first);
    const auto* writes=graph.GetPass(0)->GetWrites().data();
    const auto* reads=graph.GetPass(1)->GetReads().data();
    allocations=0;countAllocations=true;
    const bool compiled=graph.Compile();
    countAllocations=false;
    Check(compiled && allocations==0,"Warm graph compile rebuilt its working storage");
    auto lifetime=std::make_shared<int>(1);std::weak_ptr<int> released=lifetime;
    graph.AddPass("old callback").SetExecute([lifetime](auto*) {});
    lifetime.reset();
    graph.SetTimestampQuery(reinterpret_cast<R::TimestampQueryHandle>(uintptr_t{1}));
    graph.Reset();
    Check(released.expired() && !graph.IsCompiled() && !graph.GetPass(0) &&
        graph.GetExecutionStages().empty() && graph.GetDependencies().empty(),"Graph reset retained a callback or active plan");
    const auto newHandle=build(second);
    Check(newHandle!=oldHandle && graph.GetPass(0)->GetWrites().data()==writes &&
        graph.GetPass(1)->GetReads().data()==reads,"Graph reset did not reuse pass binding storage or resource identity");
    std::vector<R::ICommandList*> lists;
    Check(graph.ExecuteParallel(&device,lists,1) && device.Submit(lists.data(),static_cast<uint32_t>(lists.size())) && device.WaitIdle(),
        "Rebuilt graph retained an old resource, timestamp query or command state");
    for(auto* list:lists)device.DestroyCommandList(list);
    lists.clear();Check(callbacks==2,"Rebuilt graph executed stale callbacks");
    graph.Reset();
    graph.AddPass("stale handle").Read(oldHandle,R::ResourceState::UnorderedAccess).SetExecute([](auto*) {});
    Check(!graph.Compile(),"Graph reset accepted an earlier resource handle");
    graph.Reset();
    graph.AddPass("throw").SetExecute([](auto*) {throw std::runtime_error("expected graph callback failure");});
    Check(graph.Compile(),"Throwing graph did not compile");
    bool threw=false;
    try {(void)graph.ExecuteParallel(&device,lists,2);}catch(const std::runtime_error&) {threw=true;}
    Check(threw && lists.empty(),"Failed graph recording retained output commands");
    graph.Reset();
    const auto image=graph.ImportTexture("backbuffer",device.GetBackBuffer(),R::ResourceState::Present,R::ResourceState::Present);
    graph.AddPass("retry").Write(image,R::ResourceState::RenderTarget).SetExecute([&](auto*) {++callbacks;});
    Check(graph.Compile() && graph.ExecuteParallel(&device,lists,1) &&
        device.Submit(lists.data(),static_cast<uint32_t>(lists.size())) && device.WaitIdle() && device.Present(),
        "Graph failure/reset did not permit a new backbuffer submission");
    for(auto* list:lists)device.DestroyCommandList(list);
    Check(callbacks==3,"Graph retry executed a stale callback");
    for(unsigned repeat=0;repeat<2;++repeat)
    {
        graph.Reset();
        const auto resource=graph.ImportBuffer("dependencies",second,R::ResourceState::Common,R::ResourceState::Common);
        graph.AddPass("reader 0").Read(resource,R::ResourceState::ShaderResource).SetExecute([](auto*) {});
        graph.AddPass("reader 1").Read(resource,R::ResourceState::ShaderResource).Read(resource,R::ResourceState::ShaderResource).SetExecute([](auto*) {});
        graph.AddPass("writer").Write(resource,R::ResourceState::UnorderedAccess).SetExecute([](auto*) {});
        graph.AddPass("UAV reader").Read(resource,R::ResourceState::UnorderedAccess).SetExecute([](auto*) {});
        graph.AddPass("global").GlobalBarrier().SetExecute([](auto*) {});
        graph.AddPass("transition reader").Read(resource,R::ResourceState::ShaderResource).SetExecute([](auto*) {});
        Check(graph.Compile(),"Dependency rebuild failed");
        const std::array<R::RGDependency,10> expected={{{0,2},{0,4},{1,2},{1,4},{2,3},{2,4},{2,5},{3,4},{3,5},{4,5}}};
        const auto& actual=graph.GetDependencies();
        Check(actual.size()==expected.size(),"Dependency rebuild lost a reader or retained duplicate edges");
        for(size_t index=0;index<expected.size();++index)
            Check(actual[index].before==expected[index].before && actual[index].after==expected[index].after,
                "Dependency rebuild changed RAW/WAR/WAW/global ordering");
        const auto& stages=graph.GetExecutionStages();
        Check(stages.size()==5 && stages[0].passIndices==std::vector<uint32_t>{0,1},"Reader parallel stage changed");
        for(uint32_t stage=1;stage<stages.size();++stage)
            Check(stages[stage].passIndices==std::vector<uint32_t>{stage+1},"Dependent stage changed");
    }
    std::puts("PASS graph warm compile, reset capacity, callback release, resource replacement and failed-recording retry");
}
void InlineConstantsScope()
{
    Device device;R::ResourceScope own(device);
    const unsigned char code[]={1};
    auto* vertex=own.Keep(device.CreateShader({R::ShaderStage::Vertex,"main",code,sizeof(code)}));
    auto* fragment=own.Keep(device.CreateShader({R::ShaderStage::Fragment,"main",code,sizeof(code)}));
    R::ColorAttachmentDesc format;format.format=R::Format::R8G8B8A8_UNORM;format.writeMask=R::ColorWriteMask::All;
    R::GraphicsPipelineDesc desc;desc.vertexShader=vertex;desc.fragmentShader=fragment;desc.topology=R::PrimitiveTopology::TriangleList;
    desc.raster.fillMode=R::FillMode::Solid;desc.raster.cullMode=R::CullMode::None;desc.raster.frontFace=R::FrontFace::CounterClockwise;
    desc.colorAttachments=&format;desc.colorAttachmentCount=1;
    desc.layout.inlineConstantSize=4;desc.layout.inlineConstantStages=R::ShaderStageFlags::Vertex;
    auto* pipeline=own.Keep(device.CreateGraphicsPipeline(desc));
    R::TextureDesc texture;texture.width=texture.height=16;texture.format=format.format;texture.usage=R::TextureUsage::RenderTarget;
    auto* target=own.Keep(device.CreateTexture(texture));
    Check(pipeline && target,"Graphics scope fixture creation failed");
    const uint32_t value=7;
    auto* outside=own.Keep(device.AcquireCommandList());
    outside->BindGraphicsPipeline(pipeline);outside->SetInlineConstants(0,sizeof(value),&value);
    Check(!outside->Close(),"Graphics inline constants outside rendering were accepted");
    auto* inside=own.Keep(device.AcquireCommandList());
    const R::ResourceBarrierDesc barrier={nullptr,target,R::ResourceState::Undefined,R::ResourceState::RenderTarget,{}};
    inside->ResourceBarrier(&barrier,1);inside->BindGraphicsPipeline(pipeline);
    R::ColorAttachment color;color.texture=target;color.loadOp=R::LoadOp::Clear;color.storeOp=R::StoreOp::Store;
    inside->BeginRendering({&color,1,nullptr});inside->SetInlineConstants(0,sizeof(value),&value);
    inside->SetViewport({0,0,16,16,0,1});inside->SetScissor({0,0,16,16});inside->DrawInstanced(3,1,0,0);inside->EndRendering();
    Check(inside->Close() && device.PrepareCommandLists(&inside,1) && device.Submit(&inside,1) && device.WaitIdle(),
        "Graphics inline constants inside rendering no longer submit");
    std::puts("PASS graphics constants reject invalid scope before native replay and retain valid submission");
}
void BufferUpdateAlignment()
{
    struct Case {uint64_t alignment;uint32_t offset,size;bool accepted;};
    for(const auto& input:std::array<Case,7>{{{1,1,3,true},{4,1,3,false},{4,0,3,false},
        {4,1,4,false},{4,4,4,true},{4,12,4,true},{1,15,2,false}}})
    {
        Device device(input.alignment);R::ResourceScope own(device);
        Check(device.GetLimit(R::Limit::BufferUpdateAlignment)==input.alignment,"Upload alignment query changed");
        auto* buffer=own.Keep(device.CreateBuffer({16,0,R::BufferUsage::Storage,R::ResourceState::CopyDestination}));
        auto* commands=own.Keep(device.AcquireCommandList());
        const uint8_t bytes[16]={1,2,3,4};
        const bool accepted=device.UpdateBuffer(*commands,buffer,input.offset,bytes,input.size);
        Check(accepted==input.accepted,"UpdateBuffer did not enforce its queried alignment before replay");
        if(accepted)Check(commands->Close() && device.Submit(&commands,1) && device.WaitIdle(),"Valid byte/aligned upload failed");
        else Check(!commands->Close(),"Rejected upload retained a valid command list");
    }
    std::puts("PASS UpdateBuffer preserves byte support and rejects device-specific alignment before replay");
}
}
int main(int argc,char** argv) try
{
    Check(argc==2,"Expected backbuffer, release, graph, inline or alignment");
    if(std::strcmp(argv[1],"backbuffer")==0)BackBufferRecovery();
    else if(std::strcmp(argv[1],"release")==0)ReleaseWithoutAllocation();
    else if(std::strcmp(argv[1],"graph")==0) {GraphImportRecovery();GraphWorkspaceReuse();}
    else if(std::strcmp(argv[1],"inline")==0)InlineConstantsScope();
    else if(std::strcmp(argv[1],"alignment")==0)BufferUpdateAlignment();
    else Check(false,"Unknown regression case");
    return 0;
}
catch(const std::exception& error)
{
    failAfter=-1;countAllocations=false;
    std::fprintf(stderr,"FAIL RhiAuditRecovery: %s\n",error.what());return 1;
}
