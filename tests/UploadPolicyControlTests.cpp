#include "Backends/Null/NullDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "RHI/UploadPool.h"
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace R=dyf::RHI;
namespace D=dyf::Diagnostics;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
namespace {
class PolicyDevice final:public dyf::Backends::NullDevice
{
public:
    PolicyDevice(){CHECK(Initialize(nullptr,{})==0);}
    bool reuse=true,suballocate=true,fail=false,throwing=false;
    unsigned changes=0;
    std::vector<std::pair<bool,bool>> nativePolicies;
    bool SetUploadMemoryPolicyNative(bool nextReuse,bool nextSuballocate) override
    {
        if(throwing)throw std::bad_alloc();
        if(fail)return false;
        ++changes;reuse=nextReuse;suballocate=nextSuballocate;return true;
    }
    R::ICommandList* AcquireCommandListNative(bool pageReuse,bool slices) override
    {nativePolicies.emplace_back(pageReuse,slices);return NullDevice::AcquireCommandListNative();}
};
D::ControlDesc Control(D::LiveId source,const char* key)
{
    const auto snapshot=D::ReadLiveSnapshot();CHECK(snapshot);
    for(const auto& control:snapshot->controls)if(control.source==source&&control.key==key)return control;
    throw std::runtime_error("Missing upload control");
}
D::ControlStatus Status(D::LiveId request)
{
    const auto snapshot=D::ReadLiveSnapshot();
    for(const auto& status:snapshot->controlStatus)if(status.request==request)return status;
    throw std::runtime_error("Missing upload request");
}
unsigned Controls(D::LiveId source)
{
    const auto snapshot=D::ReadLiveSnapshot();
    unsigned count=0;for(const auto& control:snapshot->controls)if(control.source==source)++count;return count;
}
void Lifecycle()
{
    PolicyDevice device;
    const auto deviceId=D::RegisterLiveObject(&device,{D::LiveObjectKind::Device});
    const auto a=D::AllocateLiveId(),b=D::AllocateLiveId();
    D::LiveId aRevision=0,bRevision=0;
    R::ICommandList* old=nullptr;
    {
        D::LiveFrameScope frame(deviceId,a);device.PrepareLiveControls(frame.GetKey());
        aRevision=D::GetLiveContext().appliedRevision;
        CHECK(Controls(a)==2&&std::get<bool>(Control(a,"rhi.uploadPageReuse").appliedValue));
    }
    {
        D::LiveFrameScope frame(deviceId,b);device.PrepareLiveControls(frame.GetKey());
        bRevision=D::GetLiveContext().appliedRevision;
        CHECK(Controls(b)==2&&Control(a,"rhi.uploadPageReuse").owner!=Control(b,"rhi.uploadPageReuse").owner);
        old=device.AcquireCommandList();CHECK(old&&old->Close());
    }
    const auto off=D::RequestLiveControl(Control(a,"rhi.uploadPageReuse").id,false);CHECK(off);
    D::LiveId slices=0;
    {
        D::LiveFrameScope frame(deviceId,a);device.PrepareLiveControls(frame.GetKey());
        CHECK(!device.reuse&&device.suballocate);
        CHECK(Status(off).state==D::ControlState::Applied&&Status(off).appliedFrame==frame.GetKey());
        CHECK(D::GetLiveContext().appliedRevision>aRevision);aRevision=D::GetLiveContext().appliedRevision;
        CHECK(std::get<bool>(Control(b,"rhi.uploadPageReuse").appliedValue));
        CHECK(device.PrepareCommandLists(&old,1));
        CHECK(device.nativePolicies.back()==std::make_pair(true,true));
        CHECK(device.ResetCommandList(old)&&old->Close()&&device.PrepareCommandLists(&old,1));
        CHECK(device.nativePolicies.back()==std::make_pair(false,true));
        device.DestroyCommandList(old);old=nullptr;
        slices=D::RequestLiveControl(Control(a,"rhi.uploadSuballocation").id,false);CHECK(slices);
        device.PrepareLiveControls(frame.GetKey());
        CHECK(Status(slices).state==D::ControlState::Requested&&device.suballocate);
    }
    {
        D::LiveFrameScope frame(deviceId,a);device.PrepareLiveControls(frame.GetKey());
        CHECK(!device.reuse&&!device.suballocate&&Status(slices).state==D::ControlState::Applied);
        CHECK(Status(slices).appliedFrame==frame.GetKey()&&D::GetLiveContext().appliedRevision>aRevision);
        aRevision=D::GetLiveContext().appliedRevision;
    }
    {
        D::LiveFrameScope frame(deviceId,b);device.PrepareLiveControls(frame.GetKey());
        CHECK(D::GetLiveContext().appliedRevision>bRevision);bRevision=D::GetLiveContext().appliedRevision;
        CHECK(!std::get<bool>(Control(b,"rhi.uploadPageReuse").appliedValue));
        CHECK(!std::get<bool>(Control(b,"rhi.uploadSuballocation").appliedValue));
    }
    const auto noop=D::RequestLiveControl(Control(b,"rhi.uploadPageReuse").id,false);const auto changes=device.changes;
    {
        D::LiveFrameScope frame(deviceId,b);device.PrepareLiveControls(frame.GetKey());
        CHECK(Status(noop).state==D::ControlState::Applied&&Status(noop).appliedFrame==frame.GetKey());
        CHECK(device.changes==changes&&D::GetLiveContext().appliedRevision==bRevision);
    }
    for(const bool throws:{false,true})
    {
        device.fail=!throws;device.throwing=throws;
        const auto request=D::RequestLiveControl(Control(a,"rhi.uploadPageReuse").id,true);CHECK(request);
        {D::LiveFrameScope frame(deviceId,a);device.PrepareLiveControls(frame.GetKey());
            CHECK(Status(request).state==D::ControlState::Failed&&!device.reuse&&!device.suballocate);
            CHECK(!std::get<bool>(Control(a,"rhi.uploadPageReuse").appliedValue));
            CHECK(D::GetLiveContext().appliedRevision==aRevision);}
        device.fail=device.throwing=false;
    }
    const auto on=D::RequestLiveControl(Control(a,"rhi.uploadPageReuse").id,true);CHECK(on);
    {
        D::LiveFrameScope frame(deviceId,b);auto wrong=frame.GetKey();wrong.source=a;
        device.PrepareLiveControls(wrong);device.PrepareLiveControls(frame.GetKey());
        CHECK(Status(on).state==D::ControlState::Requested&&!device.reuse);
    }
    {
        D::LiveFrameScope frame(deviceId,a);device.PrepareLiveControls(frame.GetKey());
        CHECK(device.reuse&&Status(on).state==D::ControlState::Applied&&Status(on).appliedFrame==frame.GetKey());
    }
    const auto pending=D::RequestLiveControl(Control(a,"rhi.uploadPageReuse").id,false);CHECK(pending);
    device.ReleaseLiveControls(a);
    CHECK(Controls(a)==0&&Controls(b)==2&&device.reuse&&!device.suballocate);
    CHECK(Status(pending).state==D::ControlState::Cancelled);
    device.ReleaseLiveControls(b);CHECK(Controls(b)==0);
}
void Unsupported()
{
    dyf::Backends::NullDevice device;
    const auto id=D::RegisterLiveObject(&device,{D::LiveObjectKind::Device});const auto source=D::AllocateLiveId();
    device.PrepareLiveControls({id,source,0});CHECK(Controls(source)==0);
    {
        D::LiveFrameScope frame(id,source);device.PrepareLiveControls(frame.GetKey());
        CHECK(Controls(source)==2&&!Control(source,"rhi.uploadPageReuse").writable);
        CHECK(!Control(source,"rhi.uploadPageReuse").unavailableReason.empty());
    }
    const auto request=D::RequestLiveControl(Control(source,"rhi.uploadPageReuse").id,false);CHECK(request);
    {D::LiveFrameScope frame(id,source);device.PrepareLiveControls(frame.GetKey());CHECK(Status(request).state==D::ControlState::Rejected);}
    device.ReleaseLiveControls(source);CHECK(Controls(source)==0);
}
void PageEvents()
{
    struct Page{
        uint64_t capacity;std::vector<uint8_t> bytes;uint8_t* mapped;
        explicit Page(uint64_t size):capacity(size),bytes(size),mapped(bytes.data()){}
    };
    const auto device=D::AllocateLiveId(),source=D::AllocateLiveId();
    auto pool=std::make_shared<R::Detail::UploadPagePool<Page>>([](uint64_t size){return std::make_unique<Page>(size);},128,512);
    for(const bool reuse:{false,true})for(const bool suballocate:{false,true})
    {
        pool->SetPolicy({reuse,suballocate});
        for(unsigned iteration=0;iteration<3;++iteration)
        {
            const auto before=pool->GetStatistics();D::LiveFrameKey key;D::LiveId revision=0;
            {
                D::LiveFrameScope frame(device,source);key=frame.GetKey();
                revision=D::NotifyLiveSettingsApplied(source,key,!iteration);
                R::Detail::UploadArena<Page> arena(pool);CHECK(arena.Allocate(17,16)&&arena.Allocate(8,64));
                frame.SetOutcome(D::LiveOutcome::Completed);
            }
            const auto after=pool->GetStatistics();bool found=false;const auto snapshot=D::ReadLiveSnapshot();
            for(const auto& entry:snapshot->sources)if(entry.source==source){found=true;const auto& counters=entry.latest.recorded;
                CHECK(entry.latest.key==key&&entry.latest.appliedRevision==revision);
                CHECK(counters.uploadPageAllocations==after.allocationCount-before.allocationCount);
                CHECK(counters.uploadPageReuses==after.reuseCount-before.reuseCount);
                CHECK(counters.uploadPageAllocationBytes==after.allocationBytes-before.allocationBytes);
                CHECK(counters.uploadCalls==0&&counters.uploadBytes==0);
                const auto& stage=entry.latest.stages[size_t(D::LiveStage::NativeTranslate)];CHECK(stage.bytes==0&&stage.operations==0);
                CHECK(entry.flow.events.size()==(suballocate?1:2));
                for(const auto& event:entry.flow.events)CHECK(event.kind==D::LiveEventKind::UploadMemory&&event.context.frame==key&&event.context.appliedRevision==revision&&event.bytes==128&&event.operations==1);
            }
            CHECK(found);
        }
    }
}
}
int main()try
{
    CHECK(D::SetLiveCollectorEnabled(true));Lifecycle();Unsupported();PageEvents();
    std::cout<<"Upload control source/revision, logical/native snapshot, no-op, failure, release and unsupported contracts passed\n";
    return 0;
}
catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
