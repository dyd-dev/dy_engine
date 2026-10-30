#include "../src/Platform/Profiler/CollectorInternal.h"
#include "../src/Platform/Profiler/Connection/Protocol.h"
#include "dyf/RHI.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace P=dyf::Platform;
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}

static void RhiCommandRecordingKeepsCounts()
{
    using namespace dyf::RHI;
    std::unique_ptr<IDevice> device(IDevice::Create({}));
    Check(bool(device),"RHI device creation failed");
    const auto deviceId=P::FindLiveObject(device.get());
    Check(deviceId!=0,"RHI device has no live identity");
    int sourceObject=0;
    const auto source=P::RegisterLiveObject(&sourceObject,{P::LiveObjectKind::Renderer,deviceId,deviceId,0,0,"RHI capture"});
    ResourceScope resources(*device);
    const auto record=[&](bool detail)
    {
        const auto frame=P::BeginLiveFrame(deviceId,source);
        Check(P::IsLiveDetailEnabled(frame.key)==detail,"RHI capture selection mismatch");
        auto* buffer=resources.Keep(device->CreateBuffer({16,0,BufferUsage::Storage,ResourceState::Common}));
        auto* commands=resources.Keep(device->AcquireCommandList());
        Check(buffer&&commands,"RHI capture fixture creation failed");
        const ResourceBarrierDesc barrier={buffer,nullptr,ResourceState::Common,ResourceState::CopyDestination,{}};
        commands->ResourceBarrier(&barrier,1);
        Check(commands->Close(),"RHI capture command close failed");
        Check(device->Submit(&commands,1)&&device->WaitIdle(),"RHI capture submission failed");
        P::EndLiveFrame(frame,P::LiveOutcome::Completed);
        const auto snapshot=P::ReadLiveSnapshot();
        const auto found=std::find_if(snapshot->sources.begin(),snapshot->sources.end(),[&](const auto& item){return item.source==source;});
        Check(found!=snapshot->sources.end()&&found->latest.recorded.barriers>=1,"RHI barrier count was lost");
        if(detail)Check(found->capture.available&&!found->capture.flow.events.empty(),"RHI command detail was not captured");
        else Check(found->flow.events.empty(),"RHI command detail was retained without a request");
    };
    record(false);
    Check(P::RequestLiveFrameCapture(deviceId,source)!=0,"RHI capture request was rejected");
    record(true);
}

int main()
{
    try
    {
        Check(P::SetLiveCollectorEnabled(true),"collector unavailable");
        int deviceObject=0,sourceObject=0;
        const auto device=P::RegisterLiveObject(&deviceObject,{P::LiveObjectKind::Device,0,0,0,0,"capture test"});
        const auto source=P::RegisterLiveObject(&sourceObject,{P::LiveObjectKind::Renderer,device,device,0,0,"capture source"});
        const auto lossBefore=P::ReadLiveSnapshot()->loss.events;
        const auto frame=P::BeginLiveFrame(device,source);
        Check(!P::IsLiveDetailEnabled(frame.key),"ordinary frame enabled detail");
        P::LiveEvent draw;
        draw.context=P::GetLiveContext();
        draw.kind=P::LiveEventKind::Command;
        draw.commandKind=P::LiveCommandKind::Draw;
        draw.stage=P::LiveStage::Record;
        draw.operations=1;
        draw.elements=3;
        P::RecordLiveEvent(draw);
        P::LiveGpuSpan gpu;
        gpu.id=P::AllocateLiveId();
        gpu.frame=frame.key;
        gpu.appliedRevision=P::GetLiveContext().appliedRevision;
        gpu.outcome=P::LiveOutcome::Completed;
        gpu.duration={P::LiveValueState::Ready,1.0};
        P::RecordLiveGpuSpan(gpu);
        P::EndLiveFrame(frame,P::LiveOutcome::Completed);
        const auto snapshot=P::ReadLiveSnapshot();
        const auto found=std::find_if(snapshot->sources.begin(),snapshot->sources.end(),[&](const auto& item){return item.source==source;});
        Check(found!=snapshot->sources.end(),"summary-only source is missing");
        Check(found->latest.recorded.draw==1,"summary-only frame lost a draw count");
        Check(found->flow.events.empty(),"summary-only frame retained optional event detail");
        Check(found->flow.gpuSpans.size()==1,"summary-only frame lost GPU timing detail");
        Check(snapshot->loss.events==lossBefore,"intentional detail suppression was counted as loss");
        Check(!P::RequestLiveFrameCapture(device,source+100000),"unknown capture source was accepted");
        const auto request=P::RequestLiveFrameCapture(device,source);
        Check(request!=0,"capture request was rejected");
        Check(!P::RequestLiveFrameCapture(device,source),"second pending capture was accepted");
        const auto captured=P::BeginLiveFrame(device,source);
        Check(P::IsLiveDetailEnabled(captured.key),"requested frame did not enable detail");
        draw.context=P::GetLiveContext();
        draw.id=P::AllocateLiveId();
        P::RecordLiveEvent(draw);
        P::RecordLiveLink({source,draw.id});
        P::EndLiveFrame(captured,P::LiveOutcome::Completed);
        const auto capturedSnapshot=P::ReadLiveSnapshot();
        const auto capturedSource=std::find_if(capturedSnapshot->sources.begin(),capturedSnapshot->sources.end(),[&](const auto& item){return item.source==source;});
        Check(capturedSource!=capturedSnapshot->sources.end(),"capture source disappeared");
        Check(capturedSource->latest.recorded.draw==1,"captured frame lost a draw count");
        Check(capturedSource->captureRequest==request,"capture request ID was not published");
        Check(capturedSource->capture.available,"requested frame evidence is unavailable");
        Check(capturedSource->capture.summary.key==captured.key,"captured the wrong frame");
        Check(capturedSource->capture.summary.captureRequest==request,"captured frame lost its request ID");
        Check(capturedSource->capture.flow.events.size()==1,"requested event detail was not retained");
        Check(capturedSource->capture.flow.links.size()==1,"requested link detail was not retained");
        P::LiveSnapshot roundTrip;
        std::string snapshotError;
        Check(P::Detail::DecodeSnapshot(P::Detail::EncodeSnapshot(*capturedSnapshot),roundTrip,snapshotError),
            "captured snapshot did not round-trip");
        const auto decodedSource=std::find_if(roundTrip.sources.begin(),roundTrip.sources.end(),[&](const auto& item){return item.source==source;});
        Check(decodedSource!=roundTrip.sources.end()&&decodedSource->capture.flow.events.size()==1,
            "capture detail was lost in the wire format");
        const auto following=P::BeginLiveFrame(device,source);
        Check(!P::IsLiveDetailEnabled(following.key),"capture leaked into the next frame");
        P::EndLiveFrame(following,P::LiveOutcome::Completed);
        const auto after=P::ReadLiveSnapshot();
        const auto afterSource=std::find_if(after->sources.begin(),after->sources.end(),[&](const auto& item){return item.source==source;});
        Check(afterSource!=after->sources.end()&&afterSource->captureRequest==request&&afterSource->capture.flow.events.size()==1,
            "captured detail was lost after a summary-only frame");
        const auto skipped=P::BeginLiveFrame(device,source);
        P::EndLiveFrame(skipped,P::LiveOutcome::Skipped);
        const auto retryRequest=P::RequestLiveFrameCapture(device,source);
        Check(retryRequest!=0,"capture request during retry was rejected");
        const auto retried=P::BeginLiveFrame(device,source);
        Check(retried.key==skipped.key&&!P::IsLiveDetailEnabled(retried.key),
            "capture started on a retry instead of the next new frame");
        P::EndLiveFrame(retried,P::LiveOutcome::Failed);
        const auto next=P::BeginLiveFrame(device,source);
        Check(next.key!=skipped.key&&P::IsLiveDetailEnabled(next.key),"next new frame missed capture request");
        P::EndLiveFrame(next,P::LiveOutcome::Completed);
        P::Detail::FrameCaptureRequest packet{request,device,source},decoded;
        std::string error;
        Check(P::Detail::DecodeFrameCapture(P::Detail::EncodeFrameCapture(packet),decoded,error),"capture packet did not round-trip");
        Check(decoded.request==request&&decoded.device==device&&decoded.source==source,"capture packet changed IDs");
        Check(P::Detail::LiveProtocolVersion==4,"capture protocol version was not advanced");
        RhiCommandRecordingKeepsCounts();
        std::puts("PASS summary-only and requested capture behavior");
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"Live capture test failed: %s\n",error.what());
        return 1;
    }
}
