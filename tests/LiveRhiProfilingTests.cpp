#include "dyf/Diagnostics/LiveConnection.h"
#include "dyf/RHI.h"
#include "RHI/ThreadPool.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <unordered_map>
#ifdef DY_TEST_LIVE_COMPUTE_SHADER
#include "live_compute.h"
#include "dyf/Platform/Window.h"
#endif
#ifdef DY_TEST_NULL_FENCE
#include "Backends/Null/NullDevice.h"
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define CHECK(expression) do { if(!(expression)) { std::cerr << __LINE__ << ": " #expression "\n"; return 1; } } while(false)

namespace R=dyf::RHI;
namespace D=dyf::Diagnostics;
#ifdef DY_LIVE_PROFILING_ENABLED
namespace
{
D::LiveSourceSnapshot Source(D::LiveId id)
{
    const auto snapshot=D::ReadLiveSnapshot();
    for(const auto& source:snapshot->sources)if(source.source==id)return source;
    throw std::runtime_error("Missing live source");
}
template<class Predicate> bool Eventually(Predicate predicate)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do{if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    while(std::chrono::steady_clock::now()<deadline);
    return false;
}
int BufferFlow(R::IDevice& device)
{
    const auto id=D::FindLiveObject(&device);CHECK(id);
    D::LiveFrameKey key;R::FenceHandle fence;D::LiveId submission=0;
    {
        D::LiveFrameScope frame(id,id);key=frame.GetKey();
        auto* commands=device.AcquireCommandList();CHECK(commands);
        R::BufferDesc desc;desc.size=16;desc.usage=R::BufferUsage::Vertex;desc.initialState=R::ResourceState::CopyDestination;
        auto* buffer=device.CreateBuffer(desc);CHECK(buffer);
        const uint32_t data[4]={1,2,3,4};
        CHECK(device.UpdateBuffer(*commands,buffer,0,data,16));
        CHECK(device.UpdateBuffer(*commands,buffer,4,data,4));
        const R::ResourceBarrierDesc barrier{buffer,nullptr,R::ResourceState::CopyDestination,R::ResourceState::VertexBuffer,{}};
        commands->ResourceBarrier(&barrier,1);CHECK(commands->Close());
        const auto position=commands->GetLiveCommandPosition();CHECK(position.commandList&&position.generation==1&&position.index==3);
        CHECK(device.PrepareCommandLists(&commands,1));CHECK(device.PrepareCommandLists(&commands,1));
        CHECK(commands->GetLiveCommandPosition().index==position.index);
        CHECK(device.Submit({&commands,1,nullptr,0},fence));submission=device.GetLiveSubmissionId(fence);CHECK(submission);
        device.DestroyBuffer(buffer);CHECK(D::FindLiveObject(buffer));
        device.DestroyCommandList(commands);
        frame.SetOutcome(D::LiveOutcome::Submitted);
    }
    CHECK(Eventually([&]{return Source(id).latest.completed.submissions==1;}));
    const auto source=Source(id);CHECK(source.latest.key==key);
    CHECK(source.latest.recorded.uploadCalls==2&&source.latest.recorded.uploadBytes==20);
    CHECK(source.latest.recorded.fullUploads==1&&source.latest.recorded.partialUploads==1);
    CHECK(source.latest.recorded.barriers==1);
    CHECK(source.latest.submitted.uploadCalls==2&&source.latest.completed.uploadBytes==20);
    CHECK(source.latest.submitted.submissions==1&&source.latest.completed.submissions==1);
    CHECK(source.flow.gpuSpans.size()==1&&source.flow.gpuSpans[0].end.submission==submission);
    if(device.Supports(R::Feature::TimestampQuery))
    {CHECK(source.flow.gpuSpans[0].duration.state==D::LiveValueState::Ready);CHECK(source.flow.gpuSpans[0].hasQueueInterval);}
    else CHECK(source.flow.gpuSpans[0].duration.state==D::LiveValueState::Unsupported);
    std::cout<<"Recorded/replay/submitted/completed upload contract: 20 bytes, one submission, retained after wrapper destruction.\n";
    return 0;
}
int GraphTrace(R::IDevice& device)
{
    R::ResourceScope resources(device);R::BufferDesc desc;desc.size=16;desc.usage=R::BufferUsage::Vertex;
    desc.initialState=R::ResourceState::CopyDestination;auto* buffer=resources.Keep(device.CreateBuffer(desc));CHECK(buffer);
    R::RenderGraph graph;const auto handle=graph.ImportBuffer("trace buffer",buffer,desc.initialState,R::ResourceState::VertexBuffer);
    std::atomic<bool> uploaded{false};const uint32_t values[4]={1,2,3,4};
    graph.AddPass("trace writer").Write(handle,R::ResourceState::CopyDestination).SetExecute([&](R::ICommandList* commands){
        uploaded=device.UpdateBuffer(*commands,buffer,0,values,sizeof(values));});
    for(const char* name:{"trace reader A","trace reader B"})
        graph.AddPass(name).Read(handle,R::ResourceState::VertexBuffer).SetExecute([](R::ICommandList* commands){commands->InsertDebugMarker("read");});
    CHECK(graph.Compile());const auto& dependencies=graph.GetDependencies();CHECK(dependencies.size()==2);
    CHECK(dependencies[0].before==0&&dependencies[0].after==1&&dependencies[1].before==0&&dependencies[1].after==2);
    const auto id=D::FindLiveObject(&device),sourceId=D::AllocateLiveId();D::LiveFrameKey key;
    {
        D::LiveFrameScope frame(id,sourceId);key=frame.GetKey();dyf::RHI::ThreadPool pool(2);std::vector<R::ICommandList*> lists;
        CHECK(graph.ExecuteParallel(&device,lists,pool.GetThreadCount())&&uploaded);R::FenceHandle fence;
        CHECK(device.Submit({lists.data(),static_cast<uint32_t>(lists.size()),nullptr,0},fence));
        for(auto* commands:lists)device.DestroyCommandList(commands);frame.SetOutcome(D::LiveOutcome::Submitted);
    }
    CHECK(Eventually([&]{return Source(sourceId).latest.completed.uploadBytes==sizeof(values);}));
    const auto source=Source(sourceId);D::LiveId passIds[3]{};
    for(const auto& event:source.flow.events)if(event.kind==D::LiveEventKind::CpuWork&&event.stage==D::LiveStage::CpuPrepare&&std::strncmp(event.label,"trace ",6)==0)
    {CHECK(event.context.frame==key&&event.arguments[0]>=0&&event.arguments[0]<3);passIds[event.arguments[0]]=event.id;}
    CHECK(passIds[0]&&passIds[1]&&passIds[2]);const auto resource=D::FindLiveObject(buffer);
    const auto linked=[&](D::LiveId from,D::LiveId to){for(const auto& link:source.flow.links)if(link.from==from&&link.to==to)return true;return false;};
    if(!linked(passIds[0],resource)||!linked(resource,passIds[1])||!linked(resource,passIds[2]))
    {
        std::cerr<<"Graph frame="<<key.frame<<", pass IDs="<<passIds[0]<<','<<passIds[1]<<','<<passIds[2]<<", resource="<<resource
            <<", links="<<source.flow.links.size()<<", omitted="<<source.flow.omittedLinks<<'\n';
        for(const auto& link:source.flow.links)std::cerr<<link.from<<" -> "<<link.to<<'\n';
    }
    CHECK(linked(passIds[0],resource)&&linked(resource,passIds[1])&&linked(resource,passIds[2]));
    CHECK(linked(passIds[0],passIds[1])&&linked(passIds[0],passIds[2]));
    CHECK(source.latest.recorded.uploadCalls==1&&source.latest.submitted.submissions==1&&source.latest.completed.submissions==1);
    graph.Reset();CHECK(graph.GetDependencies().empty());
    std::cout<<"RenderGraph dependency/resource links and worker frame context passed.\n";return 0;
}
#ifdef DY_TEST_LIVE_COMPUTE_SHADER
int NativeDispatch(R::IDevice& device)
{
    CHECK(device.Supports(R::Feature::Compute));R::ResourceScope resources(device);
    auto* shader=resources.Keep(device.CreateShader({R::ShaderStage::Compute,ShaderData::live_computeEntryPoint,
        ShaderData::live_compute,ShaderData::live_computeSize}));CHECK(shader);
    R::ComputePipelineDesc desc;desc.computeShader=shader;auto* pipeline=resources.Keep(device.CreateComputePipeline(desc));CHECK(pipeline);
    const auto id=D::FindLiveObject(&device),sourceId=D::AllocateLiveId();
    {
        D::LiveFrameScope frame(id,sourceId);auto* commands=device.AcquireCommandList();CHECK(commands);
        commands->BindComputePipeline(pipeline);commands->Dispatch(2,3,4);CHECK(commands->Close());
        R::FenceHandle fence;CHECK(device.Submit({&commands,1,nullptr,0},fence));device.DestroyCommandList(commands);
        frame.SetOutcome(D::LiveOutcome::Submitted);
    }
    CHECK(Eventually([&]{return Source(sourceId).latest.completed.dispatch==1;}));
    const auto source=Source(sourceId);CHECK(source.latest.recorded.dispatch==1&&source.latest.submitted.dispatch==1);
    bool dimensions=false;for(const auto& event:source.flow.events)
        if(event.commandKind==D::LiveCommandKind::Dispatch)dimensions=event.arguments[0]==2&&event.arguments[1]==3&&event.arguments[2]==4;
    CHECK(dimensions&&source.flow.gpuSpans.size()==1&&source.flow.gpuSpans[0].duration.state==D::LiveValueState::Ready);
    const auto& span=source.flow.gpuSpans[0];CHECK(span.gpuClockDomain==id&&span.hasQueueInterval&&span.endGpuNs>=span.beginGpuNs);
    if(device.Supports(R::Feature::TimestampCalibration))
    {
        CHECK(span.hasCpuInterval&&span.endCpuNs>=span.beginCpuNs);
#if defined(DY_TEST_D3D12)
        CHECK(!span.cpuCalibrationErrorKnown);
#elif defined(DY_TEST_VULKAN)
        CHECK(span.cpuCalibrationErrorKnown&&span.cpuCalibrationErrorNs>0);
#endif
    }
    std::cout<<"Native Dispatch(2,3,4), single accounting and fence-completed GPU query passed.\n";
    return 0;
}
int NativePresentContinuity(R::IDevice& device)
{
    dyf::Platform::Window window(128,128,"Live RHI / finite query continuity");CHECK(window.GetHandle());
    const auto extent=window.GetFramebufferSize();CHECK(extent.width>0&&extent.height>0);
    const auto scale=window.GetContentScale();
    std::cout<<"Native window framebuffer="<<extent.width<<'x'<<extent.height<<", content scale="<<scale.x<<','<<scale.y<<'\n';
    R::SwapchainDesc desc;desc.window=window.GetHandle();desc.format=R::Format::B8G8R8A8_UNORM;
    desc.initialWidth=static_cast<uint32_t>(extent.width);desc.initialHeight=static_cast<uint32_t>(extent.height);
    desc.presentMode=R::PresentMode::Fifo;CHECK(device.CreateSwapchain(desc));
    const auto deviceId=D::FindLiveObject(&device),sourceId=deviceId;
    bool observedReady=false;uint32_t retries=0;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    for(uint32_t i=0;i<360;)
    {
        window.PollEvents();CHECK(window.IsRunning());
        CHECK(std::chrono::steady_clock::now()<deadline);
        if(!device.BeginFrame())
        {CHECK(!device.IsLost());++retries;std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
        auto* image=device.GetBackBuffer();
        auto* commands=device.AcquireCommandList();const R::ResourceBarrierDesc barriers[2]={
            {nullptr,image,R::ResourceState::Present,R::ResourceState::RenderTarget,{}},
            {nullptr,image,R::ResourceState::RenderTarget,R::ResourceState::Present,{}}};
        commands->ResourceBarrier(barriers,2);CHECK(commands->Close());CHECK(device.Submit(&commands,1));
        CHECK(device.Present());device.DestroyCommandList(commands);
        if(++i%30==0)for(const auto& span:Source(sourceId).recentGpuSpans)
            if(span.duration.state==D::LiveValueState::Ready)observedReady=true;
    }
    CHECK(observedReady);
    CHECK(Eventually([&]{return Source(sourceId).latest.completed.submissions==1;}));
    std::cout<<"Native continuous Present: asynchronous GPU samples progress while rendering continues; readiness retries="<<retries<<".\n";
#if defined(_WIN32)
    D::LiveConnectionScope connection;D::LiveClient client;bool connected=false;
    CHECK(Eventually([&]{for(const auto& target:D::LiveClient::Discover())
        if(target.processId==GetCurrentProcessId()){connected=client.Connect(target);return connected;}return false;}));
    uint64_t received=0,ready=0;bool truncated=false;
    const bool receivedReady=Eventually([&]{const auto snapshot=client.Poll();if(!snapshot)return false;
        ++received;truncated=truncated||snapshot->loss.transferTruncated;
        for(const auto& source:snapshot->sources)if(source.source==sourceId)
            for(const auto& span:source.recentGpuSpans)if(span.duration.state==D::LiveValueState::Ready)++ready;
        return ready!=0;});
    if(!receivedReady)std::cerr<<"LiveClient state="<<static_cast<int>(client.GetState())<<", error="<<client.GetLastError()
        <<", received="<<received<<", ready="<<ready<<", truncated="<<truncated<<'\n';
    CHECK(receivedReady);
    client.Disconnect();std::cout<<"LiveClient IPC preserves Ready GPU samples from the finite native run.\n";
#endif
    return 0;
}
#endif

#ifdef DY_TEST_NULL_FENCE
class ProbeDevice final:public dyf::Backends::NullDevice
{
public:
    ProbeDevice(){if(Initialize(nullptr,{})!=0)throw std::runtime_error("Null fixture initialization failed");}
    std::atomic<bool> release{true},lost{false};bool failPrepare=false,failSubmit=false,failFrame=false;
    ~ProbeDevice() override {StopLiveProfiling();}
    uint64_t GetCompletedSubmissionNative() override {return release?NullDevice::GetCompletedSubmissionNative():0;}
    R::ICommandList* AcquireCommandListNative() override {return failPrepare?nullptr:NullDevice::AcquireCommandListNative();}
    bool SubmitNative(R::ICommandList** lists,uint32_t count) override {return !failSubmit&&NullDevice::SubmitNative(lists,count);}
    bool BeginFrameNative() override {return !failFrame&&NullDevice::BeginFrameNative();}
    bool IsLostNative() const override {return lost||NullDevice::IsLostNative();}
};
int AcquireRetryGroup()
{
    ProbeDevice device;R::SwapchainDesc desc;desc.format=R::Format::R8G8B8A8_UNORM;desc.initialWidth=desc.initialHeight=8;
    CHECK(device.CreateSwapchain(desc));auto* initial=device.AcquireCommandList();device.DestroyCommandList(initial);
    const auto id=D::FindLiveObject(&device);const auto before=D::ReadLiveSnapshot();
    device.failFrame=true;for(unsigned i=0;i<10000;++i)CHECK(!device.BeginFrame());
    const auto pending=D::ReadLiveSnapshot();
    CHECK(pending->collector.retainedFrames==before->collector.retainedFrames+1);
    CHECK(pending->loss.frames==before->loss.frames);
    device.failFrame=false;CHECK(device.BeginFrame());auto* image=device.GetBackBuffer();auto* commands=device.AcquireCommandList();
    const R::ResourceBarrierDesc barriers[2]={{nullptr,image,R::ResourceState::Present,R::ResourceState::RenderTarget,{}},
        {nullptr,image,R::ResourceState::RenderTarget,R::ResourceState::Present,{}}};
    commands->ResourceBarrier(barriers,2);CHECK(commands->Close());CHECK(device.Submit(&commands,1));
    CHECK(device.Present());device.DestroyCommandList(commands);CHECK(!D::GetLiveContext().frame.frame);
    const auto source=Source(id);uint64_t frames=0;for(const auto& bucket:source.history)frames+=bucket.frameCount;CHECK(frames==1);
    bool aggregated=false;for(const auto& event:source.flow.events)
        if(event.kind==D::LiveEventKind::Wait&&event.id==source.latest.key.frame&&event.operations==10000&&event.outcome==D::LiveOutcome::Skipped)
            aggregated=event.beginNs<=event.endNs;
    CHECK(aggregated&&source.latest.recorded.waits==10000&&source.latest.outcome==D::LiveOutcome::Completed);
    std::cout<<"10,000 acquire retries retain one completed logical frame and one elapsed/attempt summary.\n";return 0;
}
int NestedAcquireRetry()
{
    ProbeDevice device;R::SwapchainDesc desc;desc.format=R::Format::R8G8B8A8_UNORM;desc.initialWidth=desc.initialHeight=8;
    CHECK(device.CreateSwapchain(desc));auto* initial=device.AcquireCommandList();device.DestroyCommandList(initial);
    const auto id=D::FindLiveObject(&device),sourceId=D::AllocateLiveId();
    {
        D::LiveFrameScope frame(id,sourceId);device.failFrame=true;for(unsigned i=0;i<4;++i)CHECK(!device.BeginFrame());
        device.failFrame=false;CHECK(device.BeginFrame());auto* image=device.GetBackBuffer();auto* commands=device.AcquireCommandList();
        const R::ResourceBarrierDesc barriers[2]={{nullptr,image,R::ResourceState::Present,R::ResourceState::RenderTarget,{}},
            {nullptr,image,R::ResourceState::RenderTarget,R::ResourceState::Present,{}}};
        commands->ResourceBarrier(barriers,2);CHECK(commands->Close());CHECK(device.Submit(&commands,1));
        CHECK(device.Present());device.DestroyCommandList(commands);frame.SetOutcome(D::LiveOutcome::Completed);
    }
    CHECK(!D::GetLiveContext().frame.frame);const auto source=Source(sourceId);
    CHECK(source.latest.outcome==D::LiveOutcome::Completed&&source.latest.recorded.waits==4);
    std::cout<<"Nested owner scope: retry propagation followed by successful Present remains Completed.\n";return 0;
}
int DeferredBudgetAndCounters()
{
    ProbeDevice device;auto* commands=device.AcquireCommandList();const auto id=D::FindLiveObject(&device);
    const auto before=D::ReadLiveSnapshot()->collector.retainedDetailBytes;
    for(unsigned i=0;i<100;++i)commands->GlobalBarrier();
    CHECK(D::ReadLiveSnapshot()->collector.retainedDetailBytes>before);
    CHECK(device.ResetCommandList(commands));CHECK(D::ReadLiveSnapshot()->collector.retainedDetailBytes<=before);
    struct Budget
    {
        size_t bytes=0;
        ~Budget(){D::ReleaseLiveDetailBytes(bytes);}
        void Release(){D::ReleaseLiveDetailBytes(bytes);bytes=0;}
    } held;
    while(D::ReserveLiveDetailBytes(1024*1024))held.bytes+=1024*1024;
    CHECK(held.bytes<=32u*1024u*1024u);
    for(unsigned i=0;i<10000;++i)commands->GlobalBarrier();CHECK(commands->Close());
    CHECK(commands->GetLiveCommandPosition().index==10000);
    CHECK(D::ReadLiveSnapshot()->collector.retainedDetailBytes<=32u*1024u*1024u);held.Release();
    R::FenceHandle fence;CHECK(device.Submit({&commands,1,nullptr,0},fence));device.DestroyCommandList(commands);
    CHECK(Eventually([&]{return Source(id).latest.completed.barriers==10000;}));
    const auto source=Source(id);
    CHECK(source.latest.recorded.barriers==10000&&source.latest.submitted.barriers==10000);
    CHECK(source.latest.incomplete&&source.flow.omittedEvents>0);
    CHECK(D::ReadLiveSnapshot()->collector.retainedDetailBytes<=32u*1024u*1024u);
    std::cout<<"Shared deferred-detail budget: capacity released on reset; all 10,000 commands survive detail omission and wrapper destruction.\n";return 0;
}
int MixedOriginsAndLateCompletion()
{
    ProbeDevice device;device.release=false;
    auto* first=device.AcquireCommandList();const auto id=D::FindLiveObject(&device);CHECK(id);
    device.DestroyCommandList(first);
    const auto a=D::AllocateLiveId(),b=D::AllocateLiveId();
    R::ICommandList* lists[2]{};D::LiveFrameKey origins[2];D::LiveId revisions[2]{};
    for(uint32_t i=0;i<2;++i)
    {
        D::LiveFrameScope frame(id,i?b:a);origins[i]=frame.GetKey();
        revisions[i]=D::NotifyLiveSettingsApplied(i?b:a,origins[i],true);
        lists[i]=device.AcquireCommandList();lists[i]->GlobalBarrier();CHECK(lists[i]->Close());
        frame.SetOutcome(D::LiveOutcome::Observed);
    }
    R::FenceHandle fence;CHECK(device.Submit({lists,2,nullptr,0},fence));const auto submission=device.GetLiveSubmissionId(fence);CHECK(submission);
    for(uint32_t i=0;i<2;++i)
    {
        const auto source=Source(i?b:a);CHECK(source.flow.gpuSpans.size()==1);
        const auto& span=source.flow.gpuSpans[0];CHECK(span.frame==origins[i]&&span.appliedRevision==revisions[i]);
        CHECK(span.outcome==D::LiveOutcome::Pending&&span.end.submission==submission);
        bool linked=false;for(const auto& link:source.flow.links)if(link.to==span.id)linked=true;CHECK(linked);
        CHECK(source.latest.completed.barriers==0);
        device.DestroyCommandList(lists[i]);
    }
    CHECK(!device.IsComplete(fence));device.release=true;
    CHECK(Eventually([&]{return Source(a).latest.completed.barriers==1&&Source(b).latest.completed.barriers==1;}));
    for(uint32_t i=0;i<2;++i)
    {const auto source=Source(i?b:a);CHECK(source.latest.key==origins[i]);CHECK(source.flow.gpuSpans[0].appliedRevision==revisions[i]);}
    std::cout<<"Mixed source/frame/revision origins, immediate pending spans/links, and late completion after DestroyCommandList passed.\n";
    return 0;
}
int FailureAndGeneration()
{
    ProbeDevice device;auto* list=device.AcquireCommandList();const auto id=D::FindLiveObject(&device);
    const auto sourceId=D::AllocateLiveId();
    for(bool prepare:{true,false})
    {
        D::LiveFrameScope frame(id,sourceId);CHECK(device.ResetCommandList(list));
        const auto before=list->GetLiveCommandPosition();list->GlobalBarrier();CHECK(list->Close());
        device.failPrepare=prepare;device.failSubmit=!prepare;R::FenceHandle fence;
        CHECK(!device.Submit({&list,1,nullptr,0},fence));CHECK(!fence&&!device.GetLiveSubmissionId(fence));
        CHECK(list->GetLiveCommandPosition().index==before.index+1);frame.SetOutcome(D::LiveOutcome::Failed);
    }
    auto source=Source(sourceId);CHECK(source.latest.submitted.submissions==0&&source.latest.completed.barriers==0);
    device.failPrepare=device.failSubmit=false;
    CHECK(device.ResetCommandList(list));const auto old=list->GetLiveCommandPosition();
    list->EndRendering();CHECK(!list->Close());CHECK(list->GetLiveCommandPosition().index==old.index);
    CHECK(device.ResetCommandList(list));CHECK(list->GetLiveCommandPosition().generation==old.generation+1);
    device.DestroyCommandList(list);
    std::cout<<"Preparation/submission rejection, invalid recording, and reset generations passed.\n";
    return 0;
}
int RawOutputAndBorrowedIdentity()
{
    ProbeDevice device;R::SwapchainDesc desc;desc.format=R::Format::R8G8B8A8_UNORM;desc.initialWidth=desc.initialHeight=8;
    CHECK(device.CreateSwapchain(desc));std::unordered_map<const void*,D::LiveId> images;
    for(int round=0;round<4;++round)
    {
        CHECK(device.BeginFrame());auto* image=device.GetBackBuffer();CHECK(image);
        const auto id=D::FindLiveObject(image);CHECK(id);if(images.count(image))CHECK(images[image]==id);else images[image]=id;
        auto* list=device.AcquireCommandList();
        const R::ResourceBarrierDesc barriers[2]={{nullptr,image,R::ResourceState::Present,R::ResourceState::RenderTarget,{}},
            {nullptr,image,R::ResourceState::RenderTarget,R::ResourceState::Present,{}}};
        list->ResourceBarrier(barriers,2);CHECK(list->Close());R::FenceHandle fence;CHECK(device.Submit({&list,1,nullptr,0},fence));
        const auto submission=device.GetLiveSubmissionId(fence);CHECK(device.Present());device.DestroyCommandList(list);
        bool output=false;for(const auto& event:Source(D::FindLiveObject(&device)).flow.events)
            if(event.kind==D::LiveEventKind::Output&&event.commandKind==D::LiveCommandKind::Present&&event.submission==submission)output=true;
        CHECK(output);
    }
    device.failFrame=true;device.lost=true;CHECK(!device.BeginFrame());bool failed=false;
    for(const auto& event:Source(D::FindLiveObject(&device)).flow.events)
        if(event.kind==D::LiveEventKind::Output&&event.outcome==D::LiveOutcome::Failed)failed=true;
    CHECK(failed);
    std::cout<<"Raw Present/failure events precede frame closure; swapchain resource identity survives borrowing generations.\n";
    return 0;
}
int PendingBounds()
{
    ProbeDevice device;device.release=false;D::LiveId id=0;R::FenceHandle last;
    for(uint32_t i=0;i<4100;++i)
    {
        auto* list=device.AcquireCommandList();id=D::FindLiveObject(&device);list->GlobalBarrier();CHECK(list->Close());
        CHECK(device.Submit({&list,1,nullptr,0},last));device.DestroyCommandList(list);
    }
    CHECK(!device.GetLiveSubmissionId(last));const auto source=Source(id);bool omitted=false;
    for(const auto& event:source.flow.events)if(event.outcome==D::LiveOutcome::Skipped&&event.reason==D::LiveSplitReason::Capacity)omitted=true;
    CHECK(omitted&&source.flow.gpuSpans.size()==1&&source.flow.gpuSpans[0].duration.state==D::LiveValueState::Unavailable);
    device.release=true;
    std::cout<<"4096 submission-ID / 1024 pending-submission limits report unavailable completion instead of invented zero timing.\n";
    return 0;
}
int DrawArgumentsAndPhysicalTexture()
{
    ProbeDevice device;auto* registration=device.AcquireCommandList();const auto id=D::FindLiveObject(&device);
    device.DestroyCommandList(registration);R::ResourceScope resources(device);
    const char binary[]="null";
    auto* vertex=resources.Keep(device.CreateShader({R::ShaderStage::Vertex,"main",binary,sizeof(binary)}));
    auto* fragment=resources.Keep(device.CreateShader({R::ShaderStage::Fragment,"main",binary,sizeof(binary)}));
    auto* mesh=resources.Keep(device.CreateShader({R::ShaderStage::Mesh,"main",binary,sizeof(binary)}));CHECK(vertex&&fragment&&mesh);
    R::ColorAttachmentDesc color{R::Format::R8G8B8A8_UNORM,{},R::ColorWriteMask::All};
    R::GraphicsPipelineDesc graphics;graphics.vertexShader=vertex;graphics.fragmentShader=fragment;
    graphics.topology=R::PrimitiveTopology::TriangleList;graphics.colorAttachments=&color;graphics.colorAttachmentCount=1;
    graphics.raster={R::FillMode::Solid,R::CullMode::None,R::FrontFace::CounterClockwise,0,0,0};
    auto* pipeline=resources.Keep(device.CreateGraphicsPipeline(graphics));CHECK(pipeline);
    R::MeshPipelineDesc meshDesc;meshDesc.meshShader=mesh;meshDesc.fragmentShader=fragment;meshDesc.raster=graphics.raster;
    meshDesc.colorAttachments=&color;meshDesc.colorAttachmentCount=1;
    auto* meshPipeline=resources.Keep(device.CreateMeshPipeline(meshDesc));CHECK(meshPipeline);
    R::TextureDesc textureDesc;textureDesc.width=textureDesc.height=8;textureDesc.format=color.format;textureDesc.usage=R::TextureUsage::RenderTarget;
    auto* texture=resources.Keep(device.CreateTexture(textureDesc));CHECK(texture);
    R::BufferDesc indexDesc;indexDesc.size=12;indexDesc.usage=R::BufferUsage::Index;indexDesc.initialState=R::ResourceState::CopyDestination;
    auto* indices=resources.Keep(device.CreateBuffer(indexDesc));CHECK(indices);
    const auto sourceId=D::AllocateLiveId();R::FenceHandle fence;
    {
        D::LiveFrameScope frame(id,sourceId);auto* commands=device.AcquireCommandList();
        const uint32_t data[3]={1,2,3};CHECK(device.UpdateBuffer(*commands,indices,0,data,12));
        const R::ResourceBarrierDesc barriers[2]={{indices,nullptr,R::ResourceState::CopyDestination,R::ResourceState::IndexBuffer,{}},
            {nullptr,texture,R::ResourceState::Undefined,R::ResourceState::RenderTarget,{}}};
        commands->ResourceBarrier(barriers,2);R::ColorAttachment target;target.texture=texture;target.loadOp=R::LoadOp::Clear;target.storeOp=R::StoreOp::Store;
        commands->BeginRendering({&target,1,nullptr});commands->BindGraphicsPipeline(pipeline);
        commands->SetViewport({0,0,8,8,0,1});commands->SetScissor({0,0,8,8});
        commands->DrawInstanced(3,2,0,5);commands->BindIndexBuffer(indices,R::Format::R32_UINT,0);
        commands->DrawIndexedInstanced(3,4,0,-1,2);commands->BindGraphicsPipeline(meshPipeline);commands->DispatchMesh(2,3,4);
        commands->EndRendering();CHECK(commands->Close());CHECK(device.Submit({&commands,1,nullptr,0},fence));
        device.DestroyCommandList(commands);frame.SetOutcome(D::LiveOutcome::Submitted);
    }
    CHECK(Eventually([&]{return Source(sourceId).latest.completed.submissions==1;}));
    const auto source=Source(sourceId);CHECK(source.latest.recorded.draw==1&&source.latest.recorded.drawIndexed==1&&source.latest.recorded.dispatchMesh==1);
    CHECK(source.latest.recorded.instances==6&&source.latest.submitted.instances==6&&source.latest.completed.instances==6);
    bool draw=false,indexed=false,dispatch=false;
    for(const auto& event:source.flow.events)
    {
        if(event.commandKind==D::LiveCommandKind::Draw)draw=event.arguments[0]==3&&event.arguments[1]==2&&event.arguments[3]==5;
        if(event.commandKind==D::LiveCommandKind::DrawIndexed)indexed=event.arguments[1]==4&&event.arguments[3]==-1&&event.arguments[4]==2;
        if(event.commandKind==D::LiveCommandKind::DispatchMesh)dispatch=event.arguments[0]==2&&event.arguments[1]==3&&event.arguments[2]==4;
    }
    CHECK(draw&&indexed&&dispatch);
    const auto lifetimeSource=D::AllocateLiveId();
    {
        D::LiveFrameScope frame(id,lifetimeSource);auto* value=device.CreateTexture(textureDesc);CHECK(value);
        device.DestroyTexture(value);frame.SetOutcome(D::LiveOutcome::Completed);
    }
    const auto lifetime=Source(lifetimeSource);CHECK(lifetime.latest.recorded.objectsCreated==1&&lifetime.latest.recorded.objectsDestroyed==1);
    std::cout<<"Exact draw/indexed/mesh arguments, six instances without replay duplication, and one physical texture retirement passed.\n";
    return 0;
}
#endif
}
#endif

int main()
{
    using namespace dyf::RHI;
    std::unique_ptr<IDevice> device(IDevice::Create({}));
    CHECK(device);
    TimestampCalibration calibration;
    calibration.cpuTicks = 123;
    calibration.cpuFrequency = 456;
    calibration.maximumDeviationNanoseconds = 789;
    if(!device->Supports(Feature::TimestampCalibration))
    {
        CHECK(!device->GetTimestampCalibration(calibration));
        CHECK(calibration.cpuDomain == CpuTimestampDomain::Unsupported);
        CHECK(calibration.cpuTicks == 0 && calibration.cpuFrequency == 0);
        CHECK(calibration.gpuTicks == 0 && calibration.gpuValidBits == 0);
        CHECK(!calibration.maximumDeviationNanoseconds);
        std::cout << "Clock calibration unsupported; no zero-duration sample published.\n";
    }
    else
    {
#if defined(_WIN32)
        LARGE_INTEGER before, after, frequency;
        CHECK(QueryPerformanceFrequency(&frequency));
        CHECK(QueryPerformanceCounter(&before));
        CHECK(device->GetTimestampCalibration(calibration));
        CHECK(QueryPerformanceCounter(&after));
        CHECK(calibration.cpuDomain == CpuTimestampDomain::QueryPerformanceCounter);
        CHECK(calibration.cpuFrequency == static_cast<uint64_t>(frequency.QuadPart));
        CHECK(calibration.cpuTicks >= static_cast<uint64_t>(before.QuadPart));
        CHECK(calibration.cpuTicks <= static_cast<uint64_t>(after.QuadPart));
#else
        CHECK(device->GetTimestampCalibration(calibration));
#endif
        CHECK(device->Supports(Feature::TimestampQuery));
        CHECK(calibration.gpuValidBits > 0 && calibration.gpuValidBits <= 64);
        CHECK(std::isfinite(calibration.gpuPeriodNanoseconds) && calibration.gpuPeriodNanoseconds > 0);
        CHECK(calibration.gpuValidBits == device->GetTimestampValidBits());
        CHECK(calibration.gpuPeriodNanoseconds == device->GetTimestampPeriodNanoseconds());
#if defined(DY_TEST_D3D12)
        CHECK(!calibration.maximumDeviationNanoseconds);
#elif defined(DY_TEST_VULKAN)
        CHECK(calibration.maximumDeviationNanoseconds && *calibration.maximumDeviationNanoseconds > 0);
#endif
        std::cout << "Native clock pair: GPU ticks=" << calibration.gpuTicks
                  << ", QPC ticks=" << calibration.cpuTicks << ", QPC Hz=" << calibration.cpuFrequency
                  << ", GPU ns/tick=" << calibration.gpuPeriodNanoseconds << '\n';
    }
    std::cout << "Live RHI clock calibration contracts passed.\n";
#ifdef DY_LIVE_PROFILING_ENABLED
    CHECK(BufferFlow(*device)==0);
    CHECK(GraphTrace(*device)==0);
#ifdef DY_TEST_LIVE_COMPUTE_SHADER
    CHECK(NativeDispatch(*device)==0);
    CHECK(NativePresentContinuity(*device)==0);
#endif
#ifdef DY_TEST_NULL_FENCE
    int failures=0;
    failures+=AcquireRetryGroup();failures+=NestedAcquireRetry();failures+=DeferredBudgetAndCounters();
    failures+=MixedOriginsAndLateCompletion();failures+=FailureAndGeneration();
    failures+=RawOutputAndBorrowedIdentity();failures+=PendingBounds();
    failures+=DrawArgumentsAndPhysicalTexture();
    if(failures)return 1;
#endif
#else
    auto* commands=device->AcquireCommandList();CHECK(!commands->GetLiveCommandPosition().commandList);
    CHECK(commands->Close());FenceHandle fence;CHECK(device->Submit({&commands,1,nullptr,0},fence));
    CHECK(!device->GetLiveSubmissionId(fence));device->DestroyCommandList(commands);
#endif
}
