#include "dyf/RHI.h"
#include "dyf/Platform/RenderDocCapture.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#define CHECK(expression) do { if(!(expression)) { std::cerr << __LINE__ << ": " #expression "\n"; return 1; } } while(false)

int main()
{
    using namespace dyf::RHI;
    std::unique_ptr<IDevice> device(IDevice::Create({}));
    CHECK(device);
    auto* commands = device->AcquireCommandList();
    CHECK(commands);
    // The name's original allocation must not survive until native replay.
    {
        std::string name(256, 'x');
        commands->BeginDebugEvent(name.c_str());
        commands->InsertDebugMarker(name.c_str());
    }
    commands->EndDebugEvent();
    CHECK(commands->Close());
    FenceHandle completion;
    CHECK(device->Submit({&commands,1,nullptr,0},completion));
    CHECK(device->Wait(completion,UINT64_MAX));
    CHECK(device->ResetCommandList(commands));
    commands->EndDebugEvent();
    CHECK(!commands->Close());
    CHECK(!device->Submit({&commands,1,nullptr,0},completion));
    CHECK(device->ResetCommandList(commands));
    commands->BeginDebugEvent("unclosed");
    CHECK(!commands->Close());
    CHECK(device->ResetCommandList(commands));

    const auto initial = device->GetResourceAllocationCounters();
    BufferDesc bufferDesc; bufferDesc.size=16; bufferDesc.usage=BufferUsage::Vertex;
    bufferDesc.initialState=ResourceState::CopyDestination;
    auto* buffer=device->CreateBuffer(bufferDesc);
    CHECK(buffer);
    CHECK(device->GetResourceAllocationCounters().buffers.live==initial.buffers.live+1);
    const uint32_t data[4]={1,2,3,4};
    CHECK(device->UpdateBuffer(*commands,buffer,0,data,sizeof(data)));
    device->DestroyBuffer(buffer);
    // The recording still owns the resource, so DestroyBuffer must not decrement early.
    CHECK(device->GetResourceAllocationCounters().buffers.live==initial.buffers.live+1);
    CHECK(commands->Close());
    CHECK(device->Submit({&commands,1,nullptr,0},completion));
    CHECK(device->Wait(completion,UINT64_MAX));
    CHECK(device->ResetCommandList(commands));
    const auto released=device->GetResourceAllocationCounters();
    CHECK(released.buffers.live==initial.buffers.live);
    CHECK(released.buffers.created==initial.buffers.created+1);
    CHECK(released.buffers.destroyed==initial.buffers.destroyed+1);

    CHECK(!device->CreateTimestampQuery({0}));
    if(device->Supports(Feature::TimestampQuery))
    {
        CHECK(device->GetTimestampPeriodNanoseconds()>0);
        CHECK(device->GetTimestampValidBits()>0 && device->GetTimestampValidBits()<=64);
        auto* query=device->CreateTimestampQuery({4});
        CHECK(query);
        uint64_t ticks[4]={};
        CHECK(!device->ReadTimestamps(query,0,2,ticks));
        CHECK(!device->ReadTimestamps(query,3,2,ticks));
        CHECK(!device->ReadTimestamps(query,0,2,nullptr));
        for(int round=0;round<3;++round)
        {
            commands->ResetTimestamps(query,0,4);
            commands->WriteTimestamp(query,0);
            commands->InsertDebugMarker("between timestamps");
            commands->WriteTimestamp(query,1);
            CHECK(commands->Close());
            CHECK(device->Submit({&commands,1,nullptr,0},completion));
            // D3D12 readback memory is protected by the submission fence. Vulkan
            // can publish query availability before the rest of the submission completes.
#if defined(DY_TEST_D3D12)
            if(device->ReadTimestamps(query,0,2,ticks)) CHECK(device->IsComplete(completion));
#endif
            CHECK(device->Wait(completion,UINT64_MAX));
            CHECK(device->ReadTimestamps(query,0,2,ticks));
            CHECK(!device->ReadTimestamps(query,2,1,ticks+2));
            const auto bits=device->GetTimestampValidBits();
            const auto mask=bits==64 ? UINT64_MAX : (uint64_t{1}<<bits)-1;
            CHECK(std::isfinite(double((ticks[1]-ticks[0])&mask)*device->GetTimestampPeriodNanoseconds()));
            CHECK(device->ResetCommandList(commands));
        }
        // A reset without writes leaves no readable sample (and must not resolve unwritten slots).
        commands->ResetTimestamps(query,0,4);
        CHECK(commands->Close());
        CHECK(device->Submit({&commands,1,nullptr,0},completion));
        CHECK(device->Wait(completion,UINT64_MAX));
        CHECK(!device->ReadTimestamps(query,0,1,ticks));
        CHECK(device->ResetCommandList(commands));
        commands->WriteTimestamp(query,4);
        CHECK(!commands->Close());
        CHECK(device->ResetCommandList(commands));
        device->DestroyTimestampQuery(query);
        std::cout << "Native timestamp query/readback/fence/reuse checks passed.\n";
    }
    else CHECK(!device->CreateTimestampQuery({2}));
    device->DestroyCommandList(commands);
    CHECK(device->WaitIdle());
    std::cout << "Profiling command and resource lifetime checks passed.\n";
}
