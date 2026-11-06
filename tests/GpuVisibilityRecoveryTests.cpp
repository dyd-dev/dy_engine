#include "dyf/GpuVisibility.h"
#include "dyf/GpuShadowVisibility.h"
#include "dyf/RHI.h"
#include "Backends/Null/NullDevice.h"
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>

namespace {
using namespace dyf;
using namespace dyf::RHI;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}

// Records RHI lifetime/state validation only; native GPU output is checked by GpuRangeVisibilityTests.
struct Commands final:ICommandList {
    void ResourceBarrierNative(const ResourceBarrierDesc*,uint32_t) override {}
    void GlobalBarrierNative() override {}
    void BeginRenderingNative(const RenderingDesc&) override {}
    void EndRenderingNative() override {}
    void BindGraphicsPipelineNative(PipelineHandle) override {}
    void BindComputePipelineNative(PipelineHandle) override {}
    void DispatchNative(uint32_t,uint32_t,uint32_t) override {}
    void BindResourceSetNative(ResourceSetHandle) override {}
    void BindVertexBufferNative(uint32_t,BufferHandle,uint32_t) override {}
    void BindIndexBufferNative(BufferHandle,Format,uint32_t) override {}
    void SetInlineConstantsNative(uint32_t,uint32_t,const void*) override {}
    void SetViewportNative(const Viewport&) override {}
    void SetScissorNative(const Rect&) override {}
    void SetStencilReferenceNative(uint32_t) override {}
    void DrawInstancedNative(uint32_t,uint32_t,uint32_t,uint32_t) override {}
    void DrawIndexedInstancedNative(uint32_t,uint32_t,uint32_t,int32_t,uint32_t) override {}
    void ResetTimestampsNative(TimestampQueryHandle,uint32_t,uint32_t) override {}
    void WriteTimestampNative(TimestampQueryHandle,uint32_t) override {}
    bool CloseNative() override {return true;}
};

struct Device final:Backends::NullDevice {
    struct ComputePipeline:Pipeline {
        explicit ComputePipeline(const PipelineLayoutDesc& layout):Pipeline(layout,true) {}
        ~ComputePipeline() override=default;
    };
    int failCreate=-1,failUpload=-1;
    bool throwFailure=false,hit=false;
    unsigned sets=0,shaders=0;
    uint64_t submitted=0;
    Device() {Check(Initialize(nullptr,{})==0,"Null initialization");}
    ~Device() override {ReleaseResources();}
    bool Fail(int& countdown) {
        if(countdown<0 || countdown--!=0)return false;
        hit=true;if(throwFailure)throw std::bad_alloc{};return true;
    }
    bool SupportsNative(Feature f) const override {return f==Feature::Compute || NullDevice::SupportsNative(f);}
    BufferHandle CreateBufferNative(const BufferDesc& d) override {return Fail(failCreate)?nullptr:NullDevice::CreateBufferNative(d);}
    TextureHandle CreateTextureNative(const TextureDesc& d) override {return Fail(failCreate)?nullptr:NullDevice::CreateTextureNative(d);}
    ShaderHandle CreateShaderNative(const ShaderDesc& d) override {
        if(Fail(failCreate))return nullptr;
        auto* result=NullDevice::CreateShaderNative(d);if(result)++shaders;return result;
    }
    PipelineHandle CreateComputePipelineNative(const ComputePipelineDesc& d) override {
        return Fail(failCreate)?nullptr:new ComputePipeline(d.layout);
    }
    ResourceSetHandle CreateResourceSetNative(const ResourceSetDesc& d) override {
        if(Fail(failCreate))return nullptr;
        auto* result=NullDevice::CreateResourceSetNative(d);if(result)++sets;return result;
    }
    void DestroyShaderNative(ShaderHandle s) override {--shaders;NullDevice::DestroyShaderNative(s);}
    void DestroyResourceSetNative(ResourceSetHandle s) override {--sets;NullDevice::DestroyResourceSetNative(s);}
    void DestroyPipelineNative(PipelineHandle p) override {
        if(auto* compute=dynamic_cast<ComputePipeline*>(p))delete compute;else NullDevice::DestroyPipelineNative(p);
    }
    bool UpdateBufferNative(ICommandList&,BufferHandle,uint32_t,const void*,uint32_t) override {return !Fail(failUpload);}
    ICommandList* AcquireCommandListNative() override {return new Commands;}
    void DiscardCommandListNative(ICommandList* c) override {delete static_cast<Commands*>(c);}
    bool SubmitNative(ICommandList** lists,uint32_t count) override {
        for(uint32_t i=0;i<count;++i)DiscardCommandListNative(lists[i]);++submitted;return true;
    }
    uint64_t GetCompletedSubmissionNative() override {return submitted;}
    uint64_t GetLastSubmissionNative() const override {return submitted;}
    void CheckEmpty() {
        Check(WaitIdle(),"device drain");const auto c=GetResourceAllocationCounters();
        Check(!c.buffers.live && !c.textures.live && !c.pipelines.live && !sets && !shaders,"recovery leaked owned resources");
    }
};

struct Fixture {
    std::vector<GpuVisibility::Instance> inputs{1};
    std::vector<GpuVisibility::Batch> batches{{6,9,0,0,1,{}}};
    std::vector<GpuVisibility::Range> ranges{
        {{-.1f,-.1f,.4f,1},{.1f,.1f,.6f,0},0,0,3,0},
        {{-.1f,-.1f,.4f,1},{.1f,.1f,.6f,0},0,3,3,0}};
    GpuVisibility::Settings settings;
    bool depthReady=false;
    Fixture() {
        inputs[0].model=inputs[0].previousMVP=Math::float4x4::Identity();inputs[0].centerRadius={0,0,.5f,.1f};
        settings.camera.view=settings.camera.projection=Math::float4x4::Identity();
        settings.width=settings.height=65;settings.lod=false;
    }
    bool Prepare(GpuVisibility& v) {return v.Prepare(inputs,batches,settings,ranges);}
};

TextureHandle Depth(Device& device) {
    TextureDesc d;d.width=65;d.height=33;d.format=Format::D32_FLOAT;
    d.usage=TextureUsage::DepthStencil|TextureUsage::ShaderResource;
    auto* result=device.CreateTexture(d);Check(result!=nullptr,"depth fixture");return result;
}

bool Frame(Device& device,GpuVisibility& visibility,Fixture& f,TextureHandle depth,bool submit,bool requireOcclusion) {
    if(!f.Prepare(visibility))return false;
    ResourceScope own(device);auto* commands=own.Keep(device.AcquireCommandList());
    Check(commands!=nullptr,"command fixture");
    if(!visibility.RecordUploads(*commands))return false;
    RenderGraph graph;if(!visibility.AddPasses(graph))return false;
    const auto image=graph.ImportTexture("depth",depth,f.depthReady?ResourceState::ShaderResource:ResourceState::Undefined,ResourceState::ShaderResource);
    graph.AddPass("depth fixture").Write(image,ResourceState::DepthWrite).SetExecute([depth](ICommandList* list) {
        DepthStencilAttachment attachment;attachment.texture=depth;attachment.state=ResourceState::DepthWrite;
        attachment.depthLoadOp=LoadOp::Clear;attachment.depthStoreOp=StoreOp::Store;
        RenderingDesc desc;desc.depthStencilAttachment=&attachment;list->BeginRendering(desc);list->EndRendering();
    });
    const bool occlusion=visibility.AddOcclusionPasses(graph,depth,image,f.settings.camera);
    if(requireOcclusion)Check(occlusion,"occlusion retry");
    if(!submit)return true;
    if(!graph.Compile() || !graph.Execute(commands) || !commands->Close() || !device.Submit(&commands,1))return false;
    visibility.CommitUploads();visibility.CommitFrame();f.depthReady=true;return true;
}

void ResourceRecovery() {
    unsigned failures=0;
    for(bool warm:{false,true})for(bool throwing:{false,true})for(int position=0;position<64;++position) {
        Device device;bool hit=false;
        {
            ResourceScope own(device);auto* depth=own.Keep(Depth(device));Fixture f;GpuVisibility v(device);
            if(warm) {
                Check(Frame(device,v,f,depth,true,true),"warm resource fixture");
                f.inputs.push_back(f.inputs[0]);f.batches[0].capacity=2;
            }
            device.failCreate=position;device.throwFailure=throwing;
            try {(void)Frame(device,v,f,depth,false,false);}catch(const std::bad_alloc&) {Check(throwing,"unexpected allocation exception");}
            hit=device.hit;device.failCreate=-1;device.throwFailure=false;
            Check(Frame(device,v,f,depth,true,true),"creation failure retry");
            Check(v.GetDrawRange(0).count==2 && v.GetArgumentsBuffer()->GetDesc().size==40,"retry lost range topology");
            Check(Frame(device,v,f,depth,true,true),"second recovered frame");
        }
        device.CheckEmpty();if(!hit)break;++failures;
    }
    Check(failures>=80,"resource fault sweep did not reach all paths");
    std::printf("PASS resource recovery: %u injected null/exception failures\n",failures);
}

void UploadRecovery() {
    for(bool warm:{false,true})for(int position=0;position<5;++position) {
        Device device;
        {
            ResourceScope own(device);auto* depth=own.Keep(Depth(device));Fixture f;GpuVisibility v(device);
            if(warm)Check(Frame(device,v,f,depth,true,true),"warm upload fixture");
            f.inputs[0].model.m[12]=.25f;f.batches[0].firstIndex=12;
            f.settings.width=67;f.ranges[0].minimum.x=-.2f;
            device.failUpload=position;
            Check(!Frame(device,v,f,depth,true,true) && device.hit,"upload fault was not reached");
            device.failUpload=-1;
            Check(Frame(device,v,f,depth,true,true),"upload retry");
        }
        device.CheckEmpty();
    }
    std::puts("PASS upload recovery: all five uploads, cold and warm");
}

void InvalidIndices(bool shadow) {
    Device device;Fixture f;f.batches[0].firstIndex=UINT32_MAX-2;
    if(shadow) {
        ResourceScope own(device);
        auto* models=own.Keep(device.CreateBuffer({sizeof(GpuShadowVisibility::Instance),sizeof(GpuShadowVisibility::Instance),BufferUsage::Storage,ResourceState::ShaderResource}));
        GpuShadowVisibility v(device);std::vector<GpuShadowVisibility::Input> inputs(1);
        std::vector<GpuShadowVisibility::View> views(1);
        Check(!v.Prepare(inputs,f.batches,views,models),"shadow accepted overflowing indirect index span");
        f.batches[0].firstIndex=9;Check(v.Prepare(inputs,f.batches,views,models),"shadow retry after invalid span");
    } else {
        GpuVisibility v(device);
        Check(!v.Prepare(f.inputs,f.batches,f.settings),"main accepted overflowing indirect index span");
        Check(!f.Prepare(v),"range path accepted overflowing indirect index span");
        f.batches[0].firstIndex=9;Check(f.Prepare(v),"main retry after invalid span");
    }
    device.CheckEmpty();std::puts("PASS indirect index span validation and retry");
}
}
int main(int argc,char** argv) try {
    Check(argc==2,"expected recovery case");
    if(!std::strcmp(argv[1],"resources"))ResourceRecovery();
    else if(!std::strcmp(argv[1],"uploads"))UploadRecovery();
    else if(!std::strcmp(argv[1],"overflow-main"))InvalidIndices(false);
    else if(!std::strcmp(argv[1],"overflow-shadow"))InvalidIndices(true);
    else throw std::runtime_error("unknown recovery case");
    return 0;
}catch(const std::exception& e) {std::fprintf(stderr,"FAIL visibility recovery: %s\n",e.what());return 1;}
