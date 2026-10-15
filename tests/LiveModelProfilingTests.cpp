#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define private public
#include "dyf/Extends/Model/ModelRenderer.h"
#include "dyf/Extends/Model/ModelScene.h"
#undef private
#include "dyf/RHI.h"
#include "dyf/Canvas.h"
#include "dyf/Image.h"
#include "dyf/Core/ThreadPool.h"
#include "Backends/Null/NullDevice.h"
namespace R=dyf::RHI;namespace D=dyf::Diagnostics;
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";return 1;}}while(false)
struct ComputePipeline:R::Pipeline{explicit ComputePipeline(const R::PipelineLayoutDesc& layout):Pipeline(layout,true){}};
struct Query:R::TimestampQuery{explicit Query(unsigned count):TimestampQuery(count){}};
// Only the native boundary is inert. Production Renderer/Model/RHI/collector execute.
struct Commands:R::ICommandList
{
    bool draw=false,dispatch=false;~Commands()override=default;
    void ResourceBarrierNative(const R::ResourceBarrierDesc*,unsigned)override{}
    void GlobalBarrierNative()override{}
    void BeginRenderingNative(const R::RenderingDesc&)override{}
    void EndRenderingNative()override{}
    void BindGraphicsPipelineNative(R::PipelineHandle)override{}
    void BindComputePipelineNative(R::PipelineHandle)override{}
    void BindResourceSetNative(R::ResourceSetHandle)override{}
    void BindVertexBufferNative(unsigned,R::BufferHandle,unsigned)override{}
    void BindIndexBufferNative(R::BufferHandle,R::Format,unsigned)override{}
    void SetInlineConstantsNative(unsigned,unsigned,const void*)override{}
    void SetViewportNative(const R::Viewport&)override{}
    void SetScissorNative(const R::Rect&)override{}
    void SetStencilReferenceNative(unsigned)override{}
    void DrawInstancedNative(unsigned,unsigned,unsigned,unsigned)override{draw=true;}
    void DrawIndexedInstancedNative(unsigned,unsigned,unsigned,int,unsigned)override{draw=true;}
    void DispatchNative(unsigned,unsigned,unsigned)override{dispatch=true;}
    void ResetTimestampsNative(R::TimestampQueryHandle,unsigned,unsigned)override{}
    void WriteTimestampNative(R::TimestampQueryHandle,unsigned)override{}
    bool CloseNative()override{return true;}
};
struct Device:dyf::Backends::NullDevice
{
    std::uint64_t completion=0;bool failDraw=false,timestamps=false,computeSupported=true,failComputePipeline=false;unsigned busyFrames=0;
    std::function<void()> onDispatch;
    explicit Device(bool compute=true):computeSupported(compute){if(Initialize(nullptr,{})!=0)std::abort();}
    ~Device(){StopLiveProfiling();ReleaseResources();}
    bool SupportsNative(R::Feature f)const override{return (f==R::Feature::Compute&&computeSupported)||(f==R::Feature::TimestampQuery&&timestamps)||NullDevice::SupportsNative(f);}
    bool CreateSwapchainNative(const R::SwapchainDesc& input)override{auto d=input;d.initialWidth=d.initialHeight=4;return NullDevice::CreateSwapchainNative(d);}
    bool BeginFrameNative()override{if(busyFrames){--busyFrames;return false;}return NullDevice::BeginFrameNative();}
    R::ICommandList* AcquireCommandListNative()override{return new Commands;}
    void DiscardCommandListNative(R::ICommandList* c)override{delete static_cast<Commands*>(c);}
    bool SubmitNative(R::ICommandList** lists,unsigned count)override{for(unsigned i=0;i<count;++i){auto* c=static_cast<Commands*>(lists[i]);if(c->dispatch&&onDispatch){auto callback=std::move(onDispatch);callback();}if(failDraw&&c->draw)return false;}for(unsigned i=0;i<count;++i)DiscardCommandListNative(lists[i]);++completion;return true;}
    std::uint64_t GetLastSubmissionNative()const override{return completion;}
    std::uint64_t GetCompletedSubmissionNative()override{return completion;}
    bool UpdateBufferNative(R::ICommandList&,R::BufferHandle,unsigned,const void*,unsigned)override{return true;}
    bool UpdateTextureNative(R::ICommandList&,R::TextureHandle,unsigned,unsigned,const void*,unsigned,unsigned,unsigned)override{return true;}
    R::PipelineHandle CreateComputePipelineNative(const R::ComputePipelineDesc& d)override{return failComputePipeline?nullptr:new ComputePipeline(d.layout);}
    R::TimestampQueryHandle CreateTimestampQueryNative(const R::TimestampQueryDesc& d)override{return new Query(d.count);}
    void DestroyTimestampQueryNative(R::TimestampQueryHandle q)override{delete static_cast<Query*>(q);}
    bool ReadTimestampsNative(R::TimestampQueryHandle,unsigned,unsigned,std::uint64_t*)override{return false;}
    double GetTimestampPeriodNative()const override{return 1;}
    unsigned GetTimestampValidBitsNative()const override{return 64;}
};
D::ControlDesc ModelControl(const D::LiveSnapshot& snapshot,const void* model)
{for(const auto& value:snapshot.controls)if(value.owner==D::FindLiveObject(model)&&value.key=="model.skinning")return value;return {};}
D::LiveSourceSnapshot Source(const D::LiveSnapshot& snapshot,D::LiveId source)
{for(const auto& value:snapshot.sources)if(value.source==source)return value;return {};}
D::ControlStatus Status(const D::LiveSnapshot& snapshot,D::LiveId request)
{for(const auto& value:snapshot.controlStatus)if(value.request==request)return value;return {};}
int ModelLiveControls(Device& device,dyf::Renderer& renderer,dyf::ModelRenderer& model,const dyf::ModelScene& scene,const dyf::ModelShaderDesc& shaders)
{
    using Mode=dyf::SkinningExecutionMode;const auto sourceId=renderer.GetLiveSourceId();auto snapshot=D::ReadLiveSnapshot();
    const auto control=ModelControl(*snapshot,&model);CHECK(control.id&&control.kind==D::ControlKind::Enumeration&&control.writable&&control.choices.size()==2);
    CHECK(control.choices[0].value==0&&control.choices[1].value==1&&control.source==sourceId);
    auto revision=Source(*snapshot,sourceId).latest.appliedRevision;auto previousMode=int(model.GetSkinningExecutionMode());
    for(const auto mode:{0,0,1}){
        const auto request=D::RequestLiveControl(control.id,std::int64_t(mode));CHECK(request&&model.Render(scene));snapshot=D::ReadLiveSnapshot();
        const auto source=Source(*snapshot,sourceId);const auto status=Status(*snapshot,request);
        const auto expected=mode==int(model.GetSkinningExecutionMode())&&std::get<std::int64_t>(ModelControl(*snapshot,&model).appliedValue)==mode;
        CHECK(expected&&status.state==D::ControlState::Applied&&status.appliedFrame==source.latest.key&&status.appliedRevision==source.latest.appliedRevision);
        CHECK(source.latest.recorded.dispatch==unsigned(mode));
        CHECK(source.latest.appliedRevision==revision+unsigned(previousMode!=mode));
        revision=source.latest.appliedRevision;previousMode=mode;
        for(const auto& span:source.flow.gpuSpans)CHECK(span.frame==source.latest.key&&span.appliedRevision==revision);
        for(const auto& item:snapshot->controls)if(item.source==sourceId&&(item.key=="instancing"||item.key=="culling"))CHECK(!item.writable&&!item.unavailableReason.empty());
    }
    CHECK(model.SetSkinningExecutionMode(Mode::VertexShader)&&model.Render(scene));snapshot=D::ReadLiveSnapshot();
    CHECK(Source(*snapshot,sourceId).latest.recorded.dispatch==0&&Source(*snapshot,sourceId).latest.appliedRevision==revision+1);
    CHECK(std::get<std::int64_t>(ModelControl(*snapshot,&model).appliedValue)==0);revision=Source(*snapshot,sourceId).latest.appliedRevision;
    CHECK(model.SetSkinningExecutionMode(Mode::VertexShader)&&model.Render(scene));CHECK(Source(*D::ReadLiveSnapshot(),sourceId).latest.appliedRevision==revision);
    D::LiveId retiredControl=0,cancelledRequest=0;
    {
        dyf::ModelRenderer fresh(renderer);auto desc=ModelControl(*D::ReadLiveSnapshot(),&fresh);CHECK(desc.id&&!desc.writable&&desc.unavailableReason.find("shader unavailable")!=std::string::npos);
        CHECK(fresh.SetShaders(shaders));CHECK(ModelControl(*D::ReadLiveSnapshot(),&fresh).writable);
        CHECK(fresh.SetShaders({}));CHECK(!ModelControl(*D::ReadLiveSnapshot(),&fresh).writable);
        CHECK(fresh.SetShaders(shaders));CHECK(ModelControl(*D::ReadLiveSnapshot(),&fresh).writable);
        device.failComputePipeline=true;const auto failed=D::RequestLiveControl(desc.id,std::int64_t(1));CHECK(failed&&fresh.Render(scene));snapshot=D::ReadLiveSnapshot();
        CHECK(fresh.GetSkinningExecutionMode()==Mode::VertexShader&&Status(*snapshot,failed).state==D::ControlState::Failed);
        CHECK(Source(*snapshot,sourceId).latest.recorded.dispatch==0&&Source(*snapshot,sourceId).latest.appliedRevision==revision);
        device.failComputePipeline=false;const auto retried=D::RequestLiveControl(desc.id,std::int64_t(1));CHECK(retried&&fresh.Render(scene));snapshot=D::ReadLiveSnapshot();
        CHECK(Status(*snapshot,retried).state==D::ControlState::Applied&&fresh.GetSkinningExecutionMode()==Mode::ComputePreSkin&&Source(*snapshot,sourceId).latest.recorded.dispatch==1);
        retiredControl=desc.id;cancelledRequest=D::RequestLiveControl(desc.id,std::int64_t(0));CHECK(cancelledRequest);
    }
    snapshot=D::ReadLiveSnapshot();CHECK(Status(*snapshot,cancelledRequest).state==D::ControlState::Cancelled);for(const auto& desc:snapshot->controls)CHECK(desc.id!=retiredControl);
    const auto foreign=D::RequestLiveControl(control.id,std::int64_t(1));CHECK(foreign);
    std::thread other([&]{D::LiveFrameScope frame(control.device,sourceId);D::PollControls(control.owner,frame.GetKey());frame.SetOutcome(D::LiveOutcome::Completed);});other.join();
    CHECK(Status(*D::ReadLiveSnapshot(),foreign).state==D::ControlState::Rejected&&model.GetSkinningExecutionMode()==Mode::VertexShader);
    {
        dyf::ModelRenderer lazy(renderer);CHECK(lazy.SetShaders(shaders));device.failComputePipeline=true;
        CHECK(lazy.SetSkinningExecutionMode(Mode::ComputePreSkin));CHECK(!lazy.Render(scene));
        device.failComputePipeline=false;CHECK(lazy.Render(scene));CHECK(Source(*D::ReadLiveSnapshot(),sourceId).latest.recorded.dispatch==1);
    }
    Device unsupported(false);auto second=dyf::Renderer::Create(unsupported,reinterpret_cast<void*>(1),renderer.GetAppliedConfig());CHECK(second);
    CHECK(second->SetShaders(renderer.GetShaders()));dyf::ModelRenderer limited(*second);CHECK(limited.SetShaders(shaders));const auto disabled=ModelControl(*D::ReadLiveSnapshot(),&limited);
    CHECK(disabled.id&&!disabled.writable&&!disabled.unavailableReason.empty());const auto rejected=D::RequestLiveControl(disabled.id,std::int64_t(1));CHECK(rejected&&limited.Render(scene));
    CHECK(Status(*D::ReadLiveSnapshot(),rejected).state==D::ControlState::Rejected&&limited.GetSkinningExecutionMode()==Mode::VertexShader);
    return 0;
}
int OptimizationControls(Device& device,dyf::Renderer& renderer,dyf::ModelRenderer& model,const dyf::ModelScene& scene)
{
    const auto sourceId=renderer.GetLiveSourceId();
    const auto toggle=[&](const char* key,bool enabled,bool modelOwned){const auto snapshot=D::ReadLiveSnapshot();for(const auto& control:snapshot->controls)if(control.source==sourceId&&control.owner==(modelOwned?D::FindLiveObject(&model):sourceId)&&control.key==key){if(control.category!=D::ControlCategory::Optimization||control.description.empty()||control.onPath.empty()||control.offPath.empty()||control.conditions.empty()||control.tradeoffs.empty())return D::LiveId(0);return D::RequestLiveControl(control.id,enabled);}return D::LiveId(0);};
    const auto applied=[&](D::LiveId request,bool enabled){const auto snapshot=D::ReadLiveSnapshot();const auto status=Status(*snapshot,request);return status.state==D::ControlState::Applied&&status.actual==D::ControlValue(enabled)&&status.previousActual==D::ControlValue(!enabled);};
    for(const auto* key:{"optimization.transformUploads","optimization.resourceSetCache","optimization.bindlessMaterials"}){
        const auto snapshot=D::ReadLiveSnapshot();bool unavailable=false;for(const auto& control:snapshot->controls)if(control.source==sourceId&&control.key==key&&!control.writable){CHECK(!control.unavailableReason.empty()&&control.category==D::ControlCategory::Optimization&&!control.onPath.empty()&&!control.offPath.empty());unavailable=true;}
        if(unavailable){CHECK(std::string(key)!="optimization.resourceSetCache");continue;}
        const auto request=toggle(key,false,false);CHECK(request&&model.Render(scene)&&applied(request,false));
        const auto restored=toggle(key,true,false);CHECK(restored&&model.Render(scene)&&applied(restored,true));
    }
    dyf::Canvas canvas(4,4);const dyf::Image image(1,1,{255,0,0,255});for(unsigned i=0;i<8;++i)canvas.Image(image,{0,0,2,2});
    CHECK(renderer.Render(canvas)&&renderer.Render(canvas));
    for(const auto* key:{"canvas.batching","canvas.imageCache","canvas.vertexBufferReuse"}){
        const auto before=device.GetResourceAllocationCounters();const auto request=toggle(key,false,false);CHECK(request&&renderer.Render(canvas)&&applied(request,false));
        const auto latest=Source(*D::ReadLiveSnapshot(),sourceId).latest;const auto after=device.GetResourceAllocationCounters();
        if(std::string(key)=="canvas.batching")CHECK(latest.recorded.draw==8);
        if(std::string(key)=="canvas.imageCache")CHECK(latest.recorded.uploadCalls>=3);
        if(std::string(key)=="canvas.vertexBufferReuse")CHECK(after.buffers.created>before.buffers.created);
        const auto restored=toggle(key,true,false);CHECK(restored&&renderer.Render(canvas)&&applied(restored,true));
    }
    CHECK(model.SetSkinningExecutionMode(dyf::SkinningExecutionMode::ComputePreSkin)&&model.Render(scene)&&model.Render(scene));
    for(const auto* key:{"model.frameBufferReuse","model.skipUnchangedUploads","model.staticMeshCache","model.influenceCache"}){
        const auto before=device.GetResourceAllocationCounters();const auto request=toggle(key,false,true);CHECK(request&&model.Render(scene)&&applied(request,false));
        const auto latest=Source(*D::ReadLiveSnapshot(),sourceId).latest;const auto after=device.GetResourceAllocationCounters();
        if(std::string(key)=="model.skipUnchangedUploads")CHECK(latest.recorded.uploadCalls>0);else CHECK(after.buffers.created>before.buffers.created);
        const auto restored=toggle(key,true,true);CHECK(restored&&model.Render(scene)&&applied(restored,true));
    }
    CHECK(model.SetSkinningExecutionMode(dyf::SkinningExecutionMode::VertexShader)&&model.Render(scene));const auto snapshot=D::ReadLiveSnapshot();bool staticUnavailable=false;for(const auto& control:snapshot->controls)if(control.owner==D::FindLiveObject(&model)&&control.key=="model.staticMeshCache")staticUnavailable=!control.writable&&!control.unavailableReason.empty();CHECK(staticUnavailable);
    return 0;
}
int main()
{
    Device device;dyf::Core::ThreadPool pool(2);dyf::RendererConfig config;config.enableProfilerHud=false;config.enableFrustumCulling=false;config.lighting.enabled=false;
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);CHECK(renderer);
    static const std::uint8_t bytes[]={1};dyf::RendererShaderDesc base;base.meshVertex={R::ShaderStage::Vertex,"main",bytes,1};base.meshFragment={R::ShaderStage::Fragment,"main",bytes,1};CHECK(renderer->SetShaders(base));
    dyf::ModelScene scene;dyf::MeshData mesh;mesh.vertices.resize(3);mesh.vertices[1].position.x=1;mesh.vertices[2].position.y=1;mesh.indices={0,1,2};CHECK(scene.Add(mesh));
    scene.m_meshSkinInfluences.resize(1);scene.m_meshSkinInfluences[0].resize(3);for(auto& influence:scene.m_meshSkinInfluences[0])influence.weights.x=1;
    scene.m_entitySkinPaletteOffsets={0};scene.m_jointPaletteMatrices.resize(1);
    dyf::ModelRenderer model(*renderer);dyf::ModelShaderDesc shaders;shaders.vertex=base.meshVertex;shaders.shadowVertex=base.meshVertex;shaders.skinningCompute={R::ShaderStage::Compute,"main",bytes,1};CHECK(model.SetShaders(shaders));CHECK(model.SetSkinningExecutionMode(dyf::SkinningExecutionMode::ComputePreSkin));CHECK(model.Render(scene));
    const auto sourceId=renderer->GetLiveSourceId();auto snapshot=D::ReadLiveSnapshot();D::LiveId control=0;
    for(const auto& c:snapshot->controls)if(c.source==sourceId&&c.key=="exposure")control=c.id;CHECK(control);
    const auto request=D::RequestLiveControl(control,2.0);CHECK(request);CHECK(model.Render(scene));
    snapshot=D::ReadLiveSnapshot();bool compute=false,main=false;
    for(const auto& source:snapshot->sources)if(source.source==sourceId){CHECK(source.latest.recorded.dispatch==1);CHECK(source.latest.objects.count==1);for(const auto& span:source.flow.gpuSpans){CHECK(span.frame==source.latest.key&&span.appliedRevision==source.latest.appliedRevision);compute|=span.scope==D::LiveGpuScope::ModelCompute;main|=span.scope==D::LiveGpuScope::RendererMainForward;}}
    CHECK(compute&&main);bool applied=false;for(const auto& status:snapshot->controlStatus)if(status.request==request)applied=status.state==D::ControlState::Applied;CHECK(applied);CHECK(renderer->GetAppliedConfig().exposure==2);
    bool queued=false;device.onDispatch=[&]{auto deferred=renderer->GetConfig();deferred.exposure=3;auto shaders=renderer->GetShaders();static const std::uint8_t fragment[]={2};shaders.meshFragment={R::ShaderStage::Fragment,"main",fragment,1};queued=renderer->SetConfig(deferred)&&renderer->SetShaders(shaders);};
    CHECK(model.Render(scene));CHECK(queued&&renderer->GetAppliedConfig().exposure==2&&renderer->GetConfig().exposure==3);
    CHECK(model.Render(scene));CHECK(renderer->GetAppliedConfig().exposure==3&&!renderer->pendingConfig);
    base.canvasVertex=base.meshVertex;base.canvasFragment=base.meshFragment;CHECK(renderer->SetShaders(base));
    dyf::Canvas canvas(4,4);const auto canvasId=D::FindLiveObject(&canvas);canvas.FillRect({0,0,1,1});canvas.FillRect({1,0,1,1});canvas.SetClipRect({0,0,2,2});canvas.FillRect({0,1,1,1});
    dyf::Image image(1,1,{255,0,0,255});canvas.Image(image,{0,0,1,1});CHECK(canvas.SetViewport({0,0,2,2}));canvas.Image(image,{0,0,1,1});
    dyf::Canvas moved(std::move(canvas));CHECK(D::FindLiveObject(&moved)==canvasId&&!D::FindLiveObject(&canvas));CHECK(renderer->Render(moved));snapshot=D::ReadLiveSnapshot();
    bool scissor=false,texture=false,viewport=false;
    for(const auto& source:snapshot->sources)if(source.source==sourceId){CHECK(source.latest.objects.count==5&&source.latest.recorded.draw==4);for(const auto& event:source.flow.events)if(event.kind==D::LiveEventKind::Batch){CHECK(event.context.object==canvasId&&event.resource&&event.stride==32);scissor|=event.reason==D::LiveSplitReason::Scissor;texture|=event.reason==D::LiveSplitReason::Texture;viewport|=event.reason==D::LiveSplitReason::Viewport;}}
    CHECK(scissor&&texture&&viewport);CHECK(renderer->Render(scene,moved));snapshot=D::ReadLiveSnapshot();for(const auto& source:snapshot->sources)if(source.source==sourceId)CHECK(source.latest.objects.count==1&&source.latest.recorded.draw==4&&source.latest.recorded.drawIndexed==1);
    device.timestamps=true;device.failDraw=true;
    for(unsigned i=0;i<20;++i)CHECK(!model.Render(scene));
    device.failDraw=false;CHECK(model.Render(scene));snapshot=D::ReadLiveSnapshot();bool recovered=false;
    for(const auto& source:snapshot->sources)if(source.source==sourceId)for(const auto& span:source.flow.gpuSpans)if(span.scope==D::LiveGpuScope::RendererMainForward&&span.begin.command.commandList)recovered=true;
    CHECK(recovered);
    device.timestamps=false;CHECK(renderer->shadersPending);CHECK(renderer->Render(moved));CHECK(!renderer->shadersPending);device.busyFrames=5000;const auto beforeBusy=D::ReadLiveSnapshot();const auto framesBefore=beforeBusy->collector.retainedFrames;
    for(const auto& source:beforeBusy->sources)if(source.source==sourceId)CHECK(source.latest.recorded.uploadCalls>0&&source.latest.submitted.submissions>0&&source.latest.recorded.draw==4);
    for(unsigned i=0;i<5000;++i)CHECK(renderer->Render(moved));CHECK(D::GetLiveContext().frame.frame==0);snapshot=D::ReadLiveSnapshot();CHECK(snapshot->collector.retainedFrames==framesBefore+1&&snapshot->loss.frames==beforeBusy->loss.frames);
    CHECK(renderer->Render(moved));snapshot=D::ReadLiveSnapshot();for(const auto& source:snapshot->sources)if(source.source==sourceId)CHECK(source.latest.outcome==D::LiveOutcome::Completed&&source.latest.recorded.draw==4&&source.latest.recorded.waits==5000);
    device.busyFrames=1000;const auto sceneBefore=D::ReadLiveSnapshot();for(unsigned i=0;i<1000;++i)CHECK(renderer->Render(scene));CHECK(renderer->Render(scene));snapshot=D::ReadLiveSnapshot();CHECK(snapshot->loss.frames==sceneBefore->loss.frames);for(const auto& source:snapshot->sources)if(source.source==sourceId)CHECK(source.latest.outcome==D::LiveOutcome::Completed&&source.latest.recorded.drawIndexed==1&&source.latest.recorded.waits==1000);
    device.busyFrames=1;CHECK(model.Render(scene));snapshot=D::ReadLiveSnapshot();for(const auto& source:snapshot->sources)if(source.source==sourceId)CHECK(source.latest.outcome==D::LiveOutcome::Skipped&&source.latest.recorded.dispatch==1&&source.latest.submitted.submissions>0);CHECK(model.Render(scene));
    const auto requestKey=[&](const char* key,D::ControlValue value){auto s=D::ReadLiveSnapshot();for(const auto& c:s->controls)if(c.source==sourceId&&c.key==key)return D::RequestLiveControl(c.id,value);return D::LiveId(0);};
    const auto unavailable=requestKey("parallelRecording",true);CHECK(unavailable&&renderer->Render(scene));snapshot=D::ReadLiveSnapshot();bool rejected=false;for(const auto& status:snapshot->controlStatus)if(status.request==unavailable)rejected=status.state==D::ControlState::Rejected;CHECK(rejected);
    base.shadowVertex=base.meshVertex;base.toneMapVertex=base.meshVertex;base.toneMapFragment=base.meshFragment;CHECK(renderer->SetShaders(base));CHECK(scene.Add(dyf::DirectionalLight{}));auto withPool=renderer->GetConfig();withPool.threadPool=&pool;CHECK(renderer->SetConfig(withPool)&&renderer->Render(scene));
    struct Case{const char* key;D::ControlValue value;};
    const Case cases[]={{"culling",true},{"culling",false},{"lighting",true},{"shadows",true},{"shadowQuality",std::int64_t(dyf::ShadowQuality::High)},{"hdr",true},{"hdr",false},{"vsync",false},{"parallelRecording",true}};
    for(const auto& test:cases){const auto id=requestKey(test.key,test.value);CHECK(id&&renderer->Render(scene));snapshot=D::ReadLiveSnapshot();bool applied=false,actual=false;for(const auto& status:snapshot->controlStatus)if(status.request==id)applied=status.state==D::ControlState::Applied&&status.appliedFrame.source==sourceId;for(const auto& c:snapshot->controls)if(c.source==sourceId&&c.key==test.key)actual=c.appliedValue==test.value;CHECK(applied&&actual);const auto& config=renderer->GetAppliedConfig();
        if(std::string(test.key)=="culling")CHECK(config.enableFrustumCulling==std::get<bool>(test.value));
        if(std::string(test.key)=="lighting")CHECK(config.lighting.enabled);
        if(std::string(test.key)=="shadows"){CHECK(config.lighting.shadows);bool shadow=false;for(const auto& source:snapshot->sources)if(source.source==sourceId)for(const auto& span:source.flow.gpuSpans)shadow|=span.scope==D::LiveGpuScope::RendererShadow;CHECK(shadow);}
        if(std::string(test.key)=="shadowQuality")CHECK(config.lighting.shadowQuality==dyf::ShadowQuality::High);
        if(std::string(test.key)=="hdr"){CHECK(config.enableHdrRendering==std::get<bool>(test.value));for(const auto& source:snapshot->sources)if(source.source==sourceId)CHECK(source.latest.recorded.objectsCreated>0||!std::get<bool>(test.value));}
        if(std::string(test.key)=="vsync")CHECK(!config.vsync);
        if(std::string(test.key)=="parallelRecording")CHECK(config.enableParallelRenderGraph&&config.threadPool==&pool);
    }
    CHECK(ModelLiveControls(device,*renderer,model,scene,shaders)==0);
    CHECK(OptimizationControls(device,*renderer,model,scene)==0);
    CHECK(renderer->Render(moved));snapshot=D::ReadLiveSnapshot();for(const auto& item:snapshot->controls)if(item.source==sourceId&&(item.key=="instancing"||item.key=="culling"))CHECK(item.writable);
    CHECK(model.Render(scene)&&renderer->Render(scene));snapshot=D::ReadLiveSnapshot();for(const auto& item:snapshot->controls)if(item.source==sourceId&&(item.key=="instancing"||item.key=="culling"))CHECK(item.writable);
    std::cout<<"Model controls/mode/revision/failure/lifetime/scope, epoch, Canvas and query recovery passed (CPU/Null)\n";
}
