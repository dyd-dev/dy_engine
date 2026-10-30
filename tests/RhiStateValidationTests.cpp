#include "dyf/RHI.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

#ifndef TEST_SHADER_PATH
#error Define TEST_SHADER_PATH to the compiled rhi_state_validation.comp SPIR-V file.
#endif

namespace
{
using namespace dyf::RHI;
void Check(bool value,const char* message)
{if(!value)throw std::runtime_error(message);}

class StateTests
{
    IDevice& device;
    ResourceScope own;

    ICommandList* Commands(){return own.Keep(device.AcquireCommandList());}
    static void Barrier(ICommandList* commands,TextureHandle texture,ResourceState before,
        ResourceState after,TextureSubresourceRange range={})
    {
        const ResourceBarrierDesc barrier{nullptr,texture,before,after,range};
        commands->ResourceBarrier(&barrier,1);
    }
    static void Barrier(ICommandList* commands,BufferHandle buffer,ResourceState before,ResourceState after)
    {
        const ResourceBarrierDesc barrier{buffer,nullptr,before,after,{}};
        commands->ResourceBarrier(&barrier,1);
    }
    void Submit(ICommandList* const* commands,uint32_t count)
    {
        FenceHandle completion;
        Check(device.Submit({commands,count,nullptr,0},completion),"Positive Submit failed.");
        Check(bool(completion),"Successful Submit returned an empty fence.");
        Check(device.Wait(completion,10000000000ull),"Submission fence timed out.");
        Check(device.IsComplete(completion),"Waited fence was not complete.");
    }
    void Submit(ICommandList* commands)
    {
        Check(commands->Close(),"Positive Close failed.");
        Submit(&commands,1);
    }
    void Reject(const char* label,ICommandList* const* commands,uint32_t count)
    {
        std::fprintf(stderr,"EXPECTED_STATE_REJECTION_BEGIN %s\n",label);
        FenceHandle completion;
        const bool submitted=device.Submit({commands,count,nullptr,0},completion);
        std::fprintf(stderr,"EXPECTED_STATE_REJECTION_END %s rejected=%u\n",label,unsigned(!submitted));
        Check(!submitted,label);
        Check(!bool(completion),"Rejected Submit returned a completion fence.");
    }
    void Reject(const char* label,ICommandList* commands)
    {
        // A state-summary mismatch is deliberately a Submit error, not a Close error.
        Check(commands->Close(),"State mismatch unexpectedly changed the Close contract.");
        Reject(label,&commands,1);
    }
    TextureHandle Texture(uint32_t mips,uint32_t layers)
    {
        TextureDesc desc;desc.width=desc.height=8;desc.mipLevels=mips;
        desc.depthOrArraySize=layers;desc.format=Format::R8G8B8A8_UNORM;
        desc.usage=TextureUsage::ShaderResource;
        return own.Keep(device.CreateTexture(desc));
    }

    void ManySubresources()
    {
        constexpr uint32_t Count=73,Mips=4,Layers=5;
        std::vector<TextureHandle> textures;
        for(uint32_t i=0;i<Count;++i)textures.push_back(Texture(Mips,Layers));
        // Reverse resource order and permuted subresources defeat a sorted-map cursor assumption.
        auto* initialize=Commands();
        for(auto image=textures.rbegin();image!=textures.rend();++image)
            for(uint32_t l=0;l<Layers;++l)for(uint32_t m=0;m<Mips;++m)
                Barrier(initialize,*image,ResourceState::Undefined,ResourceState::CopyDestination,
                    {(m*3)%Mips,1,(l*2)%Layers,1});
        Submit(initialize);

        auto* partial=Commands();
        for(auto image:textures)
            Barrier(partial,image,ResourceState::CopyDestination,ResourceState::ShaderResource,{1,2,1,3});
        Submit(partial);

        // Every one of 1,460 keys must keep its own state through growth and reuse.
        auto* verify=Commands();
        for(auto image:textures)for(uint32_t l=0;l<Layers;++l)for(uint32_t m=0;m<Mips;++m)
        {
            const bool changed=m>=1 && m<3 && l>=1 && l<4;
            Barrier(verify,image,changed?ResourceState::ShaderResource:ResourceState::CopyDestination,
                ResourceState::Common,{m,1,l,1});
        }
        Submit(verify);
        Check(device.ResetCommandList(verify),"Large completed list could not Reset.");
        for(auto image:textures)Barrier(verify,image,ResourceState::Common,ResourceState::ShaderResource);
        Submit(verify);

        auto* invalid=Commands();
        Barrier(invalid,textures[Count/2],ResourceState::Undefined,ResourceState::CopyDestination,{2,1,3,1});
        Reject("Undefined is not a wildcard for an initialized mip/layer",invalid);
        auto* unchanged=Commands();
        for(auto image:textures)Barrier(unchanged,image,ResourceState::ShaderResource,ResourceState::Common);
        Submit(unchanged);
        std::puts("PASS state tables: 1460 keys, permuted insertion, partial mip/layer transitions, Reset reuse, exact initial states.");
    }

    void OrderedRollbackAndReset()
    {
        auto* image=Texture(3,2);
        const TextureSubresourceRange first{0,1,0,1},second{2,1,1,1};
        auto* a=Commands();Barrier(a,image,ResourceState::Undefined,ResourceState::CopyDestination,first);
        auto* b=Commands();Barrier(b,image,ResourceState::CopyDestination,ResourceState::ShaderResource,first);
        auto* bad=Commands();Barrier(bad,image,ResourceState::ShaderResource,ResourceState::Common,first);
        Check(a->Close() && b->Close() && bad->Close(),"Batch fixtures did not close.");
        ICommandList* reversed[]={b,a};Reject("Reversed command-list order",reversed,2);
        ICommandList* partial[]={a,bad};Reject("Failed batch must not commit the earlier list",partial,2);
        ICommandList* ordered[]={a,b};Submit(ordered,2);

        auto* internal=Commands();
        Barrier(internal,image,ResourceState::ShaderResource,ResourceState::CopyDestination,first);
        Barrier(internal,image,ResourceState::ShaderResource,ResourceState::Common,first);
        Reject("Internal before-state mismatch",internal);
        Check(device.ResetCommandList(internal),"Rejected summary could not Reset.");
        Barrier(internal,image,ResourceState::ShaderResource,ResourceState::Common,first);
        Barrier(internal,image,ResourceState::Undefined,ResourceState::CopyDestination,second);
        Submit(internal);

        // The intermediate write is a real transition even though the final state equals the initial state.
        auto* cycle=Commands();
        Barrier(cycle,image,ResourceState::Common,ResourceState::CopyDestination,first);
        Barrier(cycle,image,ResourceState::CopyDestination,ResourceState::Common,first);
        auto* follow=Commands();Barrier(follow,image,ResourceState::Common,ResourceState::ShaderResource,first);
        Check(cycle->Close() && follow->Close(),"Cycle fixtures did not close.");
        ICommandList* cycleBatch[]={cycle,follow};Submit(cycleBatch,2);
        auto* verify=Commands();
        Barrier(verify,image,ResourceState::ShaderResource,ResourceState::Common,first);
        Barrier(verify,image,ResourceState::CopyDestination,ResourceState::ShaderResource,second);
        Barrier(verify,image,ResourceState::Undefined,ResourceState::ShaderResource,{1,1,0,1});
        Submit(verify);
        std::puts("PASS state summaries: ordered/reversed lists, rejected-batch rollback, internal mismatch, Reset, initial==final cycle.");
    }

    void BufferRollback()
    {
        auto* buffer=own.Keep(device.CreateBuffer({16,0,BufferUsage::Storage,ResourceState::UnorderedAccess}));
        auto* a=Commands();Barrier(a,buffer,ResourceState::UnorderedAccess,ResourceState::CopyDestination);
        auto* bad=Commands();Barrier(bad,buffer,ResourceState::CopySource,ResourceState::UnorderedAccess);
        Check(a->Close() && bad->Close(),"Buffer rollback fixtures did not close.");
        ICommandList* partial[]={a,bad};Reject("Buffer failed-batch rollback",partial,2);
        auto* b=Commands();Barrier(b,buffer,ResourceState::CopyDestination,ResourceState::CopySource);
        Check(b->Close(),"Buffer ordered fixture did not close.");
        ICommandList* ordered[]={a,b};Submit(ordered,2);
        std::puts("PASS buffer state summaries: failed-batch rollback and ordered retry.");
    }

    void SampledMips()
    {
        Check(device.Supports(Feature::Compute),"Vulkan compute support is required.");
        std::ifstream stream(TEST_SHADER_PATH,std::ios::binary|std::ios::ate);
        Check(bool(stream),"Cannot open TEST_SHADER_PATH.");
        const auto length=static_cast<std::streamoff>(stream.tellg());
        Check(length>0 && length<1024*1024 && length%4==0,"Invalid test SPIR-V size.");
        std::vector<uint32_t> code(static_cast<size_t>(length)/4);
        stream.seekg(0);stream.read(reinterpret_cast<char*>(code.data()),static_cast<std::streamsize>(length));
        Check(bool(stream),"Cannot read test SPIR-V.");
        auto* shader=own.Keep(device.CreateShader({ShaderStage::Compute,"main",code.data(),code.size()*4}));
        SamplerDesc sampler;sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias=0;sampler.minLod=0;sampler.maxLod=2;
        const ResourceBindingLayout layouts[]={
            {0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Compute,{}},
            {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Compute,sampler},
            {2,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}}};
        ComputePipelineDesc pipelineDesc;pipelineDesc.computeShader=shader;pipelineDesc.layout={layouts,3};
        auto* pipeline=own.Keep(device.CreateComputePipeline(pipelineDesc));
        auto* image=Texture(3,1);
        auto* output=own.Keep(device.CreateBuffer({16,0,BufferUsage::Storage,ResourceState::UnorderedAccess}));
        const ResourceBinding bindings[]={{0,0,nullptr,image,0,0,{}},{2,0,output,nullptr,0,16,{}}};
        auto* set=own.Keep(device.CreateResourceSet({pipeline,bindings,2}));
        const auto dispatch=[&](ICommandList* commands){
            commands->BindComputePipeline(pipeline);commands->BindResourceSet(set);commands->Dispatch(1,1,1);
        };

        auto* first=Commands();Barrier(first,image,ResourceState::Undefined,ResourceState::ShaderResource,{0,1,0,1});
        Submit(first);
        auto* missing=Commands();dispatch(missing);
        Reject("Full sampled view requires mip1/mip2, not only mip0",missing);

        auto* upload=Commands();
        Barrier(upload,image,ResourceState::ShaderResource,ResourceState::CopyDestination,{0,1,0,1});
        Barrier(upload,image,ResourceState::Undefined,ResourceState::CopyDestination,{1,2,0,1});
        for(uint32_t mip=0;mip<3;++mip)
        {
            const uint32_t side=8u>>mip;
            std::vector<uint32_t> pixels(side*side,0xff804020u+mip);
            Check(device.UpdateTexture(*upload,image,mip,0,pixels.data(),side*side*4,side*4,side*side*4),
                "Mip upload failed.");
        }
        Barrier(upload,image,ResourceState::CopyDestination,ResourceState::ShaderResource);
        dispatch(upload);Submit(upload);

        auto* cached=Commands();dispatch(cached);
        Barrier(cached,image,ResourceState::ShaderResource,ResourceState::CopyDestination,{1,1,0,1});
        Barrier(cached,output,ResourceState::UnorderedAccess,ResourceState::UnorderedAccess);
        // Keep the same bound set: Dispatch must recheck it after the partial barrier.
        cached->Dispatch(1,1,1);
        Reject("Partial mip barrier invalidates the cached full-view requirement",cached);
        Check(device.ResetCommandList(cached),"Rejected cached-set list could not Reset.");
        Barrier(cached,output,ResourceState::UnorderedAccess,ResourceState::UnorderedAccess);
        dispatch(cached);Submit(cached);
        std::puts("PASS sampled mip requirements: missing mips reject, full upload dispatches, partial barrier invalidates cache, rollback+Reset dispatches.");
    }
public:
    explicit StateTests(IDevice& value):device(value),own(value){}
    void Run(){ManySubresources();OrderedRollbackAndReset();BufferRollback();SampledMips();}
};
}

int main()
{
    std::unique_ptr<IDevice> device;
    try
    {
        DeviceDesc desc;desc.maxFramesInFlight=2;desc.enableValidation=false;
        device.reset(IDevice::Create(desc));Check(bool(device),"Offscreen Vulkan device creation failed.");
        Check(device->Supports(Feature::Compute),"This test must link the Vulkan backend, not Null.");
        const auto baseline=device->GetResourceAllocationCounters();
        {StateTests tests(*device);tests.Run();Check(device->WaitIdle(),"Final test WaitIdle failed.");}
        Check(device->WaitIdle(),"Post-cleanup WaitIdle failed.");
        const auto after=device->GetResourceAllocationCounters();
        Check(after.buffers.live==baseline.buffers.live && after.textures.live==baseline.textures.live &&
            after.pipelines.live==baseline.pipelines.live,"GPU resource counters leaked after ResourceScope cleanup.");
        Check(!device->IsLost(),"Vulkan device was lost.");
        std::puts("PASS RhiStateValidation: offscreen native submissions completed; ResourceScope returned all GPU counters to baseline.");
        return 0;
    }
    catch(const std::exception& error)
    {
        if(device)(void)device->WaitIdle();
        std::fprintf(stderr,"FAIL RhiStateValidation: %s\n",error.what());return 1;
    }
}
