#include "dyf/GpuShadowVisibility.h"
#include "dyf/RHI.h"
#include "ShadowInspect.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace dyf;
using namespace dyf::RHI;
namespace {
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
}
int main() try {
    DeviceDesc desc;desc.enableValidation=true;
    std::unique_ptr<IDevice> device(IDevice::Create(desc));
    Check(bool(device),"Native device creation");
    if(!GpuShadowVisibility::Supported(*device))return 77;
    ResourceScope own(*device);
    GpuShadowVisibility visibility(*device);
    std::array<GpuShadowVisibility::Instance,4> transforms={};
    for(auto& value:transforms)value.model=Math::float4x4::Identity();
    transforms[1].model=Math::Translation({-.4f,0,0});
    transforms[2].model=Math::Translation({4,0,0});
    auto* models=own.Keep(device->CreateBuffer({sizeof(transforms),128,BufferUsage::Vertex,ResourceState::CopyDestination}));
    Check(models!=nullptr,"Canonical transforms allocation");
    auto* upload=own.Keep(device->AcquireCommandList());
    Check(upload && device->UpdateBuffer(*upload,models,0,transforms.data(),sizeof(transforms)),"Canonical transforms upload");
    const ResourceBarrierDesc ready={models,nullptr,ResourceState::CopyDestination,ResourceState::VertexBuffer,{}};
    upload->ResourceBarrier(&ready,1);
    Check(upload->Close() && device->Submit(&upload,1),"Canonical transforms submission");
    std::vector<GpuShadowVisibility::Input> inputs(4);
    for(uint32_t i=0;i<inputs.size();++i) {
        inputs[i].minimum={-.1f,-.1f,.4f,1};inputs[i].maximum={.1f,.1f,.6f,0};
        inputs[i].ids={i,i==3?1u:0u,0,0};
    }
    inputs[3].minimum={std::numeric_limits<float>::quiet_NaN(),0,0,0};
    std::vector<GpuShadowVisibility::Batch> batches={{3,7,4,0,3,{}},{6,11,-2,3,1,{}}};
    std::vector<GpuShadowVisibility::View> views(2);
    views[0].projection=Math::float4x4::Identity();views[0].info={0,1,0,0};
    views[1].projection=Math::Translation({-4,0,0});views[1].info={0,1,0,0};
    Check(visibility.Prepare(inputs,batches,views,models),"GPU shadow preparation");
    const auto code=ShaderDesc{ShaderStage::Compute,ShaderData::ShadowInspectEntryPoint,ShaderData::ShadowInspect,ShaderData::ShadowInspectSize};
    auto* shader=own.Keep(device->CreateShader(code));Check(shader!=nullptr,"Inspection shader");
    const ResourceBindingLayout layout[]={{0,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {1,ResourceBindingType::StorageTexture,1,ShaderStageFlags::Compute,{}},
        {2,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}}};
    ComputePipelineDesc pipelineDesc;pipelineDesc.computeShader=shader;pipelineDesc.layout={layout,3,16,ShaderStageFlags::Compute,15};
    pipelineDesc.threadGroupSize[0]=64;
    auto* pipeline=own.Keep(device->CreateComputePipeline(pipelineDesc));Check(pipeline!=nullptr,"Inspection pipeline");
    TextureDesc texture;texture.width=64;texture.height=1;texture.format=Format::R8G8B8A8_UNORM;
    texture.usage=TextureUsage::Storage|TextureUsage::ShaderResource;
    auto* output=own.Keep(device->CreateTexture(texture));Check(output!=nullptr,"Inspection target");
    auto* payload=own.Keep(device->CreateTexture(texture));Check(payload!=nullptr,"Instance payload target");
    ResourceState outputState=ResourceState::Undefined;
    const auto inspect=[&](std::array<uint32_t,20> expected,bool checkModels=false) {
        Check(visibility.Prepare(inputs,batches,views,models),"GPU shadow re-preparation");
        auto* commands=own.Keep(device->AcquireCommandList());Check(commands!=nullptr,"GPU shadow commands");
        Check(visibility.RecordUploads(*commands),"GPU shadow uploads");
        RenderGraph graph;Check(visibility.AddPasses(graph),"GPU shadow passes");
        const auto target=graph.ImportTexture("inspection",output,outputState,ResourceState::ShaderResource);
        ResourceBinding bindings[]={{0,0,visibility.Arguments(),nullptr,0,visibility.Arguments()->GetDesc().size,{}},
            {1,0,nullptr,output,0,0,{}},{2,0,visibility.Instances(),nullptr,0,visibility.Instances()->GetDesc().size,{}}};
        auto* set=own.Keep(device->CreateResourceSet({pipeline,bindings,3}));Check(set!=nullptr,"Inspection bindings");
        graph.AddPass("inspect GPU-produced words").Read(visibility.ArgumentsResource(),ResourceState::ShaderResource)
            .Read(visibility.InstancesResource(),ResourceState::ShaderResource)
            .Write(target,ResourceState::UnorderedAccess).SetPipeline(pipeline).SetExecute([&](ICommandList* list) {
                const std::array<uint32_t,4> count={20,0,0,0};
                list->BindComputePipeline(pipeline);list->BindResourceSet(set);
                list->SetInlineConstants(0,sizeof(count),count.data());list->Dispatch(1,1,1);
            });
        if(checkModels) {
            const auto payloadTarget=graph.ImportTexture("instance payload",payload,ResourceState::Undefined,ResourceState::ShaderResource);
            bindings[1].texture=payload;
            auto* payloadSet=own.Keep(device->CreateResourceSet({pipeline,bindings,3}));Check(payloadSet!=nullptr,"Payload bindings");
            graph.AddPass("inspect compacted models").Read(visibility.ArgumentsResource(),ResourceState::ShaderResource)
                .Read(visibility.InstancesResource(),ResourceState::ShaderResource)
                .Write(payloadTarget,ResourceState::UnorderedAccess).SetPipeline(pipeline).SetExecute([&,payloadSet](ICommandList* list) {
                    const std::array<uint32_t,4> count={64,1,0,0};
                    list->BindComputePipeline(pipeline);list->BindResourceSet(payloadSet);
                    list->SetInlineConstants(0,sizeof(count),count.data());list->Dispatch(1,1,1);
                });
        }
        Check(graph.Compile() && graph.Execute(commands) && commands->Close() && device->Submit(&commands,1),"GPU shadow execution");
        visibility.CommitFrame();outputState=ResourceState::ShaderResource;
        TextureReadback readback;Check(device->ReadTexture(output,readback) && readback.pixels.size()>=80,"GPU shadow readback");
        for(uint32_t i=0;i<expected.size();++i) {
            const auto* bytes=readback.pixels.data()+i*4;
            const uint32_t actual=uint32_t(bytes[0])|(uint32_t(bytes[1])<<8)|(uint32_t(bytes[2])<<16)|(uint32_t(bytes[3])<<24);
            if(actual!=expected[i])std::fprintf(stderr,"word %u expected=%u actual=%u\n",i,expected[i],actual);
            Check(actual==expected[i],"GPU shadow culling/indirect output differs from literal oracle");
        }
        if(checkModels) {
            TextureReadback data;Check(device->ReadTexture(payload,data) && data.pixels.size()>=256,"Instance payload readback");
            const auto equal=[&](size_t slot,size_t entity) {
                return std::memcmp(data.pixels.data()+slot*sizeof(transforms[0]),&transforms[entity],sizeof(transforms[0]))==0;
            };
            Check((equal(0,0)&&equal(1,1)) || (equal(0,1)&&equal(1,0)),"GPU compaction changed/duplicated visible model payloads");
        }
    };
    inspect({3,2,7,4,0,6,1,11,UINT32_MAX-1,3,3,1,7,4,4,6,1,11,UINT32_MAX-1,7},true);
    inputs[0].minimum.x=3.9f;inputs[0].maximum.x=4.1f;
    {
        ResourceScope abandoned(*device);
        Check(visibility.Prepare(inputs,batches,views,models),"Abandoned frame preparation");
        auto* commands=abandoned.Keep(device->AcquireCommandList());Check(commands!=nullptr,"Abandoned commands");
        RenderGraph graph;
        Check(visibility.RecordUploads(*commands) && visibility.AddPasses(graph) &&
            graph.Compile() && graph.Execute(commands) && commands->Close(),"Abandoned frame recording");
        // No Submit or CommitFrame: the next attempt must still upload the dirty bounds.
    }
    inspect({3,1,7,4,0,6,1,11,UINT32_MAX-1,3,3,2,7,4,4,6,1,11,UINT32_MAX-1,7});
    inputs[0].minimum.x=-.1f;inputs[0].maximum.x=.1f;
    views[0].receiverPlanes[0]={1,0,0,0};views[0].info[0]=1;
    inspect({3,1,7,4,0,6,1,11,UINT32_MAX-1,3,3,1,7,4,4,6,1,11,UINT32_MAX-1,7});
    views[1].info[1]=0;
    inspect({3,1,7,4,0,6,1,11,UINT32_MAX-1,3,3,0,7,4,4,6,0,11,UINT32_MAX-1,7});
    views[0].projection.m[0]=std::numeric_limits<float>::quiet_NaN();
    inspect({3,3,7,4,0,6,1,11,UINT32_MAX-1,3,3,0,7,4,4,6,0,11,UINT32_MAX-1,7});
    inputs[0].ids[0]=4;
    Check(!visibility.Prepare(inputs,batches,views,models),"Out-of-range transform index accepted");
    std::puts("PASS GPU shadow culling: native indirect words, compaction, receiver planes, clean views and invalid-bound fail-open");
    return 0;
} catch(const std::exception& error) {std::fprintf(stderr,"FAIL GPU shadows: %s\n",error.what());return 1;}
