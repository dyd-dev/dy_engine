#include "dyf/GpuVisibility.h"
#include "dyf/RHI.h"
#include "RangeInspect.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
using namespace dyf;
using namespace dyf::RHI;
namespace { void Check(bool v,const char* message) {if(!v)throw std::runtime_error(message);} }
int main() try {
    DeviceDesc desc;desc.enableValidation=true;
    std::unique_ptr<IDevice> device(IDevice::Create(desc));Check(bool(device),"device");
    if(!device->Supports(Feature::Compute) || !device->Supports(Feature::IndirectFirstInstance))return 77;
    ResourceScope own(*device);GpuVisibility visibility(*device);
    std::vector<GpuVisibility::Instance> inputs(2);
    for(uint32_t i=0;i<2;++i) {
        inputs[i].model=inputs[i].previousMVP=Math::float4x4::Identity();
        inputs[i].padding0=11+i;inputs[i].centerRadius={0,0,.5f,.1f};
    }
    inputs[1].model=Math::Translation({-.4f,0,0});inputs[1].centerRadius.x=-.4f;
    std::vector<GpuVisibility::Batch> batches={{6,9,0,0,2,{}}};
    std::vector<GpuVisibility::Range> ranges={
        {{-.1f,-.1f,.4f,1},{.1f,.1f,.6f,0},0,0,3,0},
        {{4,-.1f,.4f,1},{5,.1f,.6f,0},0,3,3,0}};
    GpuVisibility::Settings settings;settings.camera.view=settings.camera.projection=Math::float4x4::Identity();
    settings.width=settings.height=64;settings.lod=false;
    auto* shader=own.Keep(device->CreateShader({ShaderStage::Compute,ShaderData::RangeInspectEntryPoint,ShaderData::RangeInspect,ShaderData::RangeInspectSize}));
    const ResourceBindingLayout layout[]={{0,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {1,ResourceBindingType::StorageTexture,1,ShaderStageFlags::Compute,{}},
        {2,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {3,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}}};
    ComputePipelineDesc pd;pd.computeShader=shader;pd.layout={layout,4,16,ShaderStageFlags::Compute,15};pd.threadGroupSize[0]=64;
    auto* pipeline=own.Keep(device->CreateComputePipeline(pd));Check(pipeline!=nullptr,"inspection pipeline");
    TextureDesc td;td.width=128;td.height=1;td.format=Format::R8G8B8A8_UNORM;td.usage=TextureUsage::Storage|TextureUsage::ShaderResource;
    auto* output=own.Keep(device->CreateTexture(td));Check(output!=nullptr,"inspection texture");
    ResourceState state=ResourceState::Undefined;
    auto inspect=[&](std::array<uint32_t,15> expected,bool keys,uint32_t keyOffset=0,uint32_t primitive=3) {
        Check(visibility.Prepare(inputs,batches,settings,ranges),"prepare ranges");
        Check(visibility.GetDrawRange(0).count==(ranges.empty()?1u:2u),"range topology");
        auto* commands=own.Keep(device->AcquireCommandList());Check(commands && visibility.RecordUploads(*commands),"uploads");
        RenderGraph graph;Check(visibility.AddPasses(graph),"range passes");
        const auto target=graph.ImportTexture("words",output,state,ResourceState::ShaderResource);
        auto* words=visibility.GetArgumentsBuffer();
        ResourceBinding bindings[]={{0,0,words,nullptr,0,words->GetDesc().size,{}},{1,0,nullptr,output,0,0,{}},
            {2,0,visibility.GetInstanceBuffer(),nullptr,0,visibility.GetInstanceBuffer()->GetDesc().size,{}},
            {3,0,visibility.GetInstanceKeysBuffer(),nullptr,0,visibility.GetInstanceKeysBuffer()->GetDesc().size,{}}};
        auto* set=own.Keep(device->CreateResourceSet({pipeline,bindings,4}));Check(set!=nullptr,"inspect bindings");
        graph.AddPass("inspect range words").Read(visibility.GetArgumentsHandle(),ResourceState::ShaderResource).Read(visibility.GetInstanceKeysHandle(),ResourceState::ShaderResource)
            .Read(visibility.GetInstancesHandle(),ResourceState::ShaderResource).Write(target,ResourceState::UnorderedAccess)
            .SetPipeline(pipeline).SetExecute([=](ICommandList* list) {
                const std::array<uint32_t,4> count={keys?68u:static_cast<uint32_t>(words->GetDesc().size/4),keys?2u:0u,keyOffset,0};list->BindComputePipeline(pipeline);
                list->BindResourceSet(set);list->SetInlineConstants(0,16,count.data());list->Dispatch(keys?2:1,1,1);
            });
        Check(graph.Compile() && graph.Execute(commands) && commands->Close() && device->Submit(&commands,1),"execute");
        visibility.CommitUploads();visibility.CommitFrame();state=ResourceState::ShaderResource;
        TextureReadback data;Check(device->ReadTexture(output,data),"readback");
        std::array<uint32_t,15> actual={};std::memcpy(actual.data(),data.pixels.data(),keys?16:words->GetDesc().size);
        if(keys) {
            Check(((actual[0]==11&&actual[2]==12)||(actual[0]==12&&actual[2]==11))&&actual[1]==primitive&&actual[3]==primitive,"stable entity and absolute primitive keys");
            for(uint32_t row=0;row<2;++row)
                Check(std::memcmp(data.pixels.data()+16+row*sizeof(GpuVisibility::VisibleInstance),&inputs[actual[row*2]-11],sizeof(GpuVisibility::VisibleInstance))==0,"compacted model/previousMVP does not match entity key");
        }
        else {for(uint32_t i=0;i<15;++i)if(actual[i]!=expected[i])std::fprintf(stderr,"word %u: %u != %u\n",i,actual[i],expected[i]);Check(actual==expected,"GPU range indirect oracle");}
    };
    inspect({3,2,9,0,0,3,0,12,0,2},false);inspect({},true);
    inputs[0].model.m[0]=-1;inputs[0].model.m[4]=3;
    inspect({3,2,9,0,0,3,0,12,0,2},false);inspect({},true);
    inputs[0].model.m[3]=.25f;
    inspect({3,2,9,0,0,3,1,12,0,2},false); // Projective transforms cannot use affine bounds.
    inputs[0].model=Math::float4x4::Identity();
    ranges[1].minimum.x=std::numeric_limits<float>::quiet_NaN();
    {ResourceScope discarded(*device);Check(visibility.Prepare(inputs,batches,settings,ranges),"retry prepare");
        auto* commands=discarded.Keep(device->AcquireCommandList());RenderGraph graph;
        Check(commands&&visibility.RecordUploads(*commands)&&visibility.AddPasses(graph)&&graph.Compile()&&graph.Execute(commands)&&commands->Close(),"unsubmitted frame");}
    inspect({3,2,9,0,0,3,2,12,0,2},false);
    inspect({},true,2,4); // Absolute batch base 9/3 plus the second range's 3/3.
    batches[0].indexCount=3;
    Check(!visibility.Prepare(inputs,batches,settings,ranges),"shrunk batch accepted previous ranges");
    Check(!visibility.Prepare(inputs,batches,settings,ranges),"failed range validation was cached as success");
    batches[0].indexCount=6;
    ranges[1].indexCount=4;Check(!visibility.Prepare(inputs,batches,settings,ranges),"invalid index capacity accepted");
    ranges[1].indexCount=3;settings.lod=true;
    batches.push_back({3,30,0,2,2,{}});
    for(auto& input:inputs) {input.lodCount=2;input.batches[1]=1;}
    inspect({3,0,9,0,0,3,0,12,0,2,3,2,30,0,4},false);
    inspect({},true,4,10);
    ranges.clear();settings.lod=false;
    inspect({6,2,9,0,0,3,0,30,0,2},false);inspect({},true);
    std::puts("PASS GPU ranges: native args, multi-instance stable keys, invalid-bound fail-open and unsubmitted retry");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL GPU ranges: %s\n",e.what());return 1;}
