#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define private public
#include <dyf/Renderer.h>
#undef private
#include "dyf/RendererInternal.h"
#include <dyf/Scene.h>
#include <dyf/Camera.h>
#include <dyf/RHI.h>
#include "Backends/Null/NullDevice.h"
#include <stdexcept>
#include <iostream>
namespace {
namespace RHI=dyf::RHI;
using dyf::Math::float4x4;
unsigned drawCount=0,instanceCount=0;
void Check(bool v,const char* m) { if(!v) throw std::runtime_error(m); }
struct Commands : RHI::ICommandList {
    bool hasDraw=false;
    ~Commands() override = default;
    void ResourceBarrierNative(const RHI::ResourceBarrierDesc*, uint32_t) override {}
    void GlobalBarrierNative() override {}
    void BeginRenderingNative(const RHI::RenderingDesc&) override {}
    void EndRenderingNative() override {}
    void BindGraphicsPipelineNative(RHI::PipelineHandle) override {}
    void BindComputePipelineNative(RHI::PipelineHandle) override {}
    void DispatchNative(uint32_t, uint32_t, uint32_t) override {}
    void BindResourceSetNative(RHI::ResourceSetHandle) override {}
    void BindVertexBufferNative(uint32_t, RHI::BufferHandle, uint32_t) override {}
    void BindIndexBufferNative(RHI::BufferHandle, RHI::Format, uint32_t) override {}
    void SetInlineConstantsNative(uint32_t, uint32_t, const void*) override {}
    void SetViewportNative(const RHI::Viewport&) override {}
    void SetScissorNative(const RHI::Rect&) override {}
    void SetStencilReferenceNative(uint32_t) override {}
    void DrawInstancedNative(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndexedInstancedNative(uint32_t, uint32_t count, uint32_t, int32_t, uint32_t) override { hasDraw=true;++drawCount; instanceCount+=count; }
    void ResetTimestampsNative(RHI::TimestampQueryHandle, uint32_t, uint32_t) override {}
    void WriteTimestampNative(RHI::TimestampQueryHandle, uint32_t) override {}
    bool CloseNative() override { return true; }
};

struct Device : dyf::Backends::NullDevice {
 unsigned buffers=0,sets=0,setsLive=0;
 uint64_t completed=0,submitted=0;
 bool delayed=false,failDraw=false,failUpload=false,throwSet=false,bindless=false;
 bool SupportsNative(RHI::Feature feature) const override {
  if(feature==RHI::Feature::IndirectDraw || feature==RHI::Feature::IndirectFirstInstance)return false;
  return feature==RHI::Feature::DescriptorIndexing ? bindless : NullDevice::SupportsNative(feature);
 }
 std::vector<RHI::ICommandList*> pending;
 ~Device() {Complete();ReleaseResources();}
 void Complete() {completed=submitted;for(auto* c:pending)DiscardCommandListNative(c);pending.clear();}
 uint64_t GetCompletedSubmissionNative() override {return completed;}
 uint64_t GetLastSubmissionNative() const override {return submitted;}

 bool UpdateTextureNative(RHI::ICommandList&,RHI::TextureHandle,uint32_t,uint32_t,const void*,uint32_t,uint32_t,uint32_t) override {return true;}
 bool CreateSwapchainNative(const RHI::SwapchainDesc& d) override { auto b=d;b.initialWidth=b.initialHeight=16;return NullDevice::CreateSwapchainNative(b); }
 Device(){Check(Initialize(nullptr,{})==0,"initialize");}
 RHI::ICommandList* AcquireCommandListNative() override { return new Commands; }
 void DiscardCommandListNative(RHI::ICommandList* c) override { delete static_cast<Commands*>(c); }
 bool SubmitNative(RHI::ICommandList** c,uint32_t n) override {
  if(failUpload) {failUpload=false;return false;}
  if(failDraw)for(uint32_t i=0;i<n;++i)if(static_cast<Commands*>(c[i])->hasDraw){failDraw=false;return false;}
  ++submitted;
  for(uint32_t i=0;i<n;++i) {if(delayed)pending.push_back(c[i]);else DiscardCommandListNative(c[i]);}
  if(!delayed)completed=submitted;
  return true;
 }
 bool UpdateBufferNative(RHI::ICommandList&,RHI::BufferHandle,uint32_t,const void*,uint32_t) override { return true; }
 RHI::BufferHandle CreateBufferNative(const RHI::BufferDesc& d) override {++buffers;return NullDevice::CreateBufferNative(d);}
 RHI::ResourceSetHandle CreateResourceSetNative(const RHI::ResourceSetDesc& d) override {++sets;if(throwSet)throw std::runtime_error("injected set failure");auto* set=NullDevice::CreateResourceSetNative(d);if(set)++setsLive;return set;}
 void DestroyResourceSetNative(RHI::ResourceSetHandle set) override {--setsLive;NullDevice::DestroyResourceSetNative(set);}
};
struct SharedScene : dyf::Scene {
 void AddCopies(const dyf::MeshData& mesh) { auto m=CreateMesh(mesh); for(int i=0;i<3;++i)(void)CreateEntity(m,CreateMaterial()); }
};
}
int main(int argc,char** argv) {
 try {
 const std::string mode=argc>1?argv[1]:"batch";
 Device device;device.bindless=mode=="bindless"; dyf::RendererConfig config; config.enableProfilerHud=false;config.enableHdrRendering=mode=="hdr";
 auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);Check(bool(renderer),"renderer");
 static const uint8_t code[]={1};
 renderer->m_impl->vertexShader=device.CreateShader({RHI::ShaderStage::Vertex,"main",code,1}); renderer->m_impl->fragmentShader=device.CreateShader({RHI::ShaderStage::Fragment,"main",code,1});
 if(mode=="hdr") {renderer->m_impl->toneVertexShader=device.CreateShader({RHI::ShaderStage::Vertex,"main",code,1});renderer->m_impl->toneFragmentShader=device.CreateShader({RHI::ShaderStage::Fragment,"main",code,1});}
 dyf::MeshData mesh;mesh.vertices.resize(3);mesh.indices={0,1,2};mesh.vertices[1].position.x=.2f;mesh.vertices[2].position.y=.2f;
 dyf::Scene scene; auto source=scene.Add(mesh); auto copy=scene.AddInstance(source); Check(bool(copy) && bool(scene.AddInstance(source)),"instances");
 Check(scene.Meshes().size()==1,"instance must share mesh");
 auto changed=copy.GetMaterial();changed.baseColor.x=.2f;Check(copy.SetMaterial(changed),"copy material edit");Check(source.GetMaterial().baseColor.x==1,"material independence");Check(copy.SetMaterial(source.GetMaterial()),"restore equal material");
 dyf::Scene foreign;Check(!foreign.AddInstance(source),"foreign handle rejected");
 dyf::Camera camera;
 auto* target=device.CreateTexture({16,16,1,1,RHI::Format::R8G8B8A8_UNORM,RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource});
 Check(target!=nullptr,"target");
 Check(renderer->RenderToTexture(scene,camera,target,RHI::ResourceState::Undefined),"first render");
 if(mode=="batch")Check(drawCount==1 && instanceCount==3,"compatible copies must be one instanced draw");
 if(mode=="reuse") {const auto b=device.buffers,s=device.sets;Check(renderer->RenderToTexture(scene,camera,target,RHI::ResourceState::ShaderResource),"warm render");Check(device.buffers==b && device.sets==s,"warm render must reuse buffers and sets");}
 if(mode=="cull") {Check(scene.GetEntity(static_cast<dyf::EntityID>(1)).SetPosition({10000,0,0}),"move");drawCount=instanceCount=0;Check(renderer->RenderToTexture(scene,camera,target,RHI::ResourceState::ShaderResource),"cull render");Check(instanceCount==2,"outside entity must be culled");}

 const auto render=[&] {drawCount=instanceCount=0;return renderer->RenderToTexture(scene,camera,target,RHI::ResourceState::ShaderResource);};
 if(mode=="cull") {
  for(uint32_t i=0;i<scene.GetEntityCount();++i)Check(scene.GetEntity(static_cast<dyf::EntityID>(i)).SetPosition({10000,0,0}),"move all outside");
  Check(render(),"all culled clear-only render");const auto stats=renderer->GetLastRenderStatistics();
  Check(stats.mainDrawCount==0 && stats.mainInstanceCount==0 && stats.culledEntityCount==scene.GetEntityCount(),"all culled statistics");
 }
 if(mode=="rotating") {
  dyf::RendererShaderDesc shaders;shaders.meshVertex={RHI::ShaderStage::Vertex,"main",code,1};shaders.meshFragment={RHI::ShaderStage::Fragment,"main",code,1};
  shaders.vertexBindings.push_back({30,RHI::ResourceBindingType::ReadOnlyStorageBuffer,1,RHI::ShaderStageFlags::Vertex,{}});
  Check(renderer->SetShaders(shaders),"rotating shader layout");
  RHI::BufferHandle slots[2];for(auto& slot:slots)slot=device.CreateBuffer({16,16,RHI::BufferUsage::Storage,RHI::ResourceState::ShaderResource});
  std::vector<dyf::RendererDrawDesc> draws(3);
  const auto frame=[&](unsigned slot) {for(auto& draw:draws)draw.vertexResources={{30,0,slots[slot],nullptr,0,16,{}}};return renderer->Render(scene,camera,draws);};
  Check(frame(0) && frame(1),"warm both descriptor slots");const auto sets=device.sets;
  for(unsigned i=0;i<6;++i)Check(frame(i%2),"rotating render");Check(device.sets==sets,"rotating live frame slots must reuse descriptor sets");
  for(auto* slot:slots)device.DestroyBuffer(slot);
 }
 if(mode=="invalid-rtt") {
  const auto previous=renderer->GetLastRenderCompletion();Check(renderer->GetLastRenderStatistics().mainDrawCount>0,"successful stats before invalid call");
  Check(!renderer->RenderToTexture(scene,camera,nullptr,RHI::ResourceState::Undefined),"null target rejected");
  auto stats=renderer->GetLastRenderStatistics();Check(stats.mainDrawCount==0 && stats.mainInstanceCount==0 && stats.culledEntityCount==0,"early RTT failure must clear latest stats");
  auto completion=renderer->GetLastRenderCompletion();Check(completion.m_device==previous.m_device && completion.m_value==previous.m_value,"invalid RTT preserves completion");
  Check(render(),"restore successful stats");const auto submitted=renderer->GetLastRenderCompletion();
  Check(!renderer->RenderToTexture(scene,camera,target,RHI::ResourceState::Present),"invalid before rejected");stats=renderer->GetLastRenderStatistics();Check(stats.mainDrawCount==0 && stats.mainInstanceCount==0 && stats.culledEntityCount==0,"invalid before clears stats");
  completion=renderer->GetLastRenderCompletion();Check(completion.m_device==submitted.m_device && completion.m_value==submitted.m_value,"invalid state preserves completion");
 }
 if(mode=="hdr") {
  const auto b=device.buffers,s=device.sets;device.delayed=true;
  for(unsigned i=0;i<4;++i)Check(render(),"warm queued HDR render");
  Check(device.buffers==b && device.sets==s,"HDR warm frames must reuse buffers and tone-map descriptor");
  auto* resized=device.CreateTexture({24,24,1,1,RHI::Format::R8G8B8A8_UNORM,RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource});
  auto* previousHdr=renderer->m_impl->hdrTarget;
  Check(renderer->RenderToTexture(scene,camera,resized,RHI::ResourceState::Undefined),"HDR resize");Check(renderer->m_impl->hdrTarget!=previousHdr,"HDR input replaced");
  const auto resizedSets=device.sets;Check(resizedSets==s+1,"resize creates exactly one replacement tone set");
  Check(renderer->RenderToTexture(scene,camera,resized,RHI::ResourceState::ShaderResource) && device.sets==resizedSets,"resized HDR descriptor reused");
  auto* different=device.CreateTexture({24,24,1,1,RHI::Format::B8G8R8A8_UNORM,RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource});
  Check(renderer->RenderToTexture(scene,camera,different,RHI::ResourceState::Undefined),"tone pipeline format change");
  const auto formatSets=device.sets;Check(formatSets==resizedSets+1,"pipeline change replaces tone descriptor");
  Check(renderer->RenderToTexture(scene,camera,different,RHI::ResourceState::ShaderResource) && device.sets==formatSets,"new pipeline descriptor reused");
  auto* previousToneSet=renderer->m_impl->toneSet;
  device.failDraw=true;Check(!renderer->RenderToTexture(scene,camera,different,RHI::ResourceState::ShaderResource),"HDR draw failure");
  Check(renderer->RenderToTexture(scene,camera,different,RHI::ResourceState::ShaderResource) && renderer->m_impl->toneSet==previousToneSet,"HDR failure retry reuses valid tone descriptor");
  auto* retryTarget=device.CreateTexture({32,32,1,1,RHI::Format::B8G8R8A8_UNORM,RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource});
  device.throwSet=true;Check(!renderer->RenderToTexture(scene,camera,retryTarget,RHI::ResourceState::Undefined),"HDR descriptor creation failure");Check(!renderer->m_impl->toneSet,"failed creation is not cached");device.throwSet=false;
  Check(renderer->RenderToTexture(scene,camera,retryTarget,RHI::ResourceState::Undefined),"HDR descriptor retry");const auto retrySets=device.sets;
  Check(renderer->RenderToTexture(scene,camera,retryTarget,RHI::ResourceState::ShaderResource) && device.sets==retrySets,"retry descriptor reused");
  const auto beforeSwap=renderer->GetLastRenderCompletion();
  dyf::RendererShaderDesc shaders;shaders.meshVertex={RHI::ShaderStage::Vertex,"main",code,1};shaders.meshFragment={RHI::ShaderStage::Fragment,"main",code,1};Check(renderer->SetShaders(shaders) && renderer->SetHdrEnabled(false),"queue HDR setting replacement");
  Check(!renderer->Render(scene,camera,std::vector<dyf::RendererDrawDesc>{}),"apply settings then reject mismatched draws");
  const auto afterSwap=renderer->GetLastRenderCompletion();Check(afterSwap.m_device==beforeSwap.m_device && afterSwap.m_value==beforeSwap.m_value,"settings resource swap preserves completion");Check(!renderer->m_impl->toneSet,"HDR setting replacement releases tone descriptor owner");
  device.DestroyTexture(retryTarget);device.DestroyTexture(resized);device.DestroyTexture(different);device.Complete();
 }
 if(mode=="edits") {
  changed.baseColor={.2f,.3f,.4f,1};Check(copy.SetMaterial(changed),"change material");Check(render(),"edited render");Check(drawCount==2 && instanceCount==3,"opaque material groups must form two batches without dropping instances");
  Check(copy.SetMaterial(source.GetMaterial()),"restore material");auto lighting=copy.GetLighting();lighting.receiveShadow=false;Check(copy.SetLighting(lighting),"lighting edit");Check(source.GetLighting().receiveShadow,"lighting independence");Check(render() && drawCount==2 && instanceCount==3,"receive-shadow must split otherwise equal opaque batches");
  Check(copy.SetLighting(source.GetLighting()),"restore lighting");Check(render() && drawCount==1,"equal copies batch again");
  Check(bool(renderer->GetLastRenderCompletion()),"draw completion published");
  const auto& stats=renderer->GetLastRenderStatistics();Check(stats.mainDrawCount==1 && stats.mainInstanceCount==3 && stats.culledEntityCount==0,"batch statistics");
 }
 if(mode=="inflight") {
  device.delayed=true;const auto b=device.buffers,s=device.sets;
  for(int frame=0;frame<5;++frame) {Check(copy.SetPosition({frame*.01f,0,0}),"animated transform");Check(render(),"queued render");Check(!device.IsComplete(renderer->GetLastRenderCompletion()),"fence must remain pending");}
  Check(device.buffers==b && device.sets==s,"queued frames must reuse immutable sets and GPU-only buffers");
  device.Complete();Check(device.IsComplete(renderer->GetLastRenderCompletion()),"completion after drain");
 }
 if(mode=="failure") {
  device.delayed=true;const auto old=renderer->GetLastRenderCompletion();device.failDraw=true;
  Check(!render(),"draw submission failure");Check(renderer->GetLastRenderStatistics().mainDrawCount==0,"failed stats not published");Check(device.IsComplete(renderer->GetLastRenderCompletion()),"failed submit preserves prior completed fence");
  Check(render(),"draw retry");Check(!device.IsComplete(renderer->GetLastRenderCompletion()),"retry draw pending");device.Complete();
  device.failUpload=true;Check(!render(),"upload failure");Check(render(),"upload retry");device.Complete();
 }
 if(mode=="fallback") {
  Check(copy.SetPosition({10000,0,0}),"outside");std::vector<dyf::RendererDrawDesc> draws(3);drawCount=instanceCount=0;
  Check(renderer->Render(scene,camera,draws),"explicit draw render");Check(instanceCount==3 && drawCount==3,"explicit draws must bypass culling and batching");
  dyf::RendererShaderDesc shaders;shaders.meshVertex={RHI::ShaderStage::Vertex,"main",code,1};shaders.meshFragment={RHI::ShaderStage::Fragment,"main",code,1};Check(renderer->SetShaders(shaders),"custom shaders");
  Check(render(),"custom render");Check(instanceCount==3 && drawCount==3,"custom shaders must bypass culling and batching");
 }
 if(mode=="bounds") {
  camera.view=float4x4::Identity();camera.projection=dyf::Math::OrthographicRH_ZO(2,2,.1f,10);
  auto world=float4x4::Identity();world.m[0]=-2;world.m[5]=.5f;world.m[12]=1;world.m[14]=-1;
  Check(source.SetTransform(world) && copy.SetTransform(world) && scene.GetEntity(static_cast<dyf::EntityID>(2)).SetTransform(world),"negative nonuniform edge transform");
  Check(render() && instanceCount==3,"boundary extent must remain visible");world.m[12]=3;Check(copy.SetTransform(world),"outside transform");Check(render() && instanceCount==2,"transformed bounds reject outside");
  camera.projection.m[3]=.2f;Check(render() && instanceCount==3,"nonstandard projection fails open");
 }
 if(mode=="handles") {
  dyf::EntityHandle expired;{dyf::Scene temporary;expired=temporary.Add(mesh);}Check(!scene.AddInstance(expired),"expired rejected");
  auto invalid=float4x4::Identity();invalid.m[0]=std::numeric_limits<float>::infinity();Check(!scene.AddInstance(source,invalid),"nonfinite rejected");Check(scene.GetEntityCount()==3,"invalid instance did not append");
 }
 if(mode=="shadow") {
  renderer->m_impl->config.lighting.shadows=true;renderer->m_impl->config.lighting.shadowQuality=dyf::ShadowQuality::Low;
  renderer->m_impl->shadowVertexShader=device.CreateShader({RHI::ShaderStage::Vertex,"main",code,1});renderer->m_impl->shadowFragmentShader=device.CreateShader({RHI::ShaderStage::Fragment,"main",code,1});
  device.DestroyPipeline(renderer->m_impl->pipeline);renderer->m_impl->pipeline=nullptr;
  dyf::DirectionalLight light;light.castShadow=true;Check(bool(scene.Add(light)),"shadow light");Check(copy.SetPosition({10000,0,0}),"offscreen caster");
  Check(render(),"shadow render");const auto& stats=renderer->GetLastRenderStatistics();
  std::printf("shadow main=%u shadow=%u native=%u views=%u\n",stats.mainInstanceCount,stats.shadowInstanceCount,instanceCount,stats.shadowViewCount);
  Check(stats.mainInstanceCount==2,"main culling");Check(instanceCount==5,"offcamera caster remains in shadow pass");
  const auto b=device.buffers,s=device.sets;Check(render(),"warm shadows");Check(device.buffers==b && device.sets==s,"shadow buffers and sets reused");
 }
 if(mode=="bindless") {
  Check(renderer->m_impl->materialIndexBuffer!=nullptr,"bindless material indices prepared");const auto b=device.buffers,s=device.sets;device.delayed=true;
  for(int i=0;i<4;++i)Check(render(),"queued bindless render");Check(device.buffers==b && device.sets==s,"bindless buffers and sets reused");
  device.failDraw=true;Check(!render(),"bindless draw failure");Check(render(),"bindless retry");device.Complete();device.delayed=false;
  Check(bool(scene.AddInstance(source)),"grow instances");auto* prior=renderer->m_impl->materialIndexBuffer;device.throwSet=true;Check(!render(),"set failure");Check(renderer->m_impl->materialIndexBuffer==prior,"failed set rolls back material buffer ownership");device.throwSet=false;Check(render(),"material replacement retry");
  dyf::Scene empty;Check(renderer->RenderToTexture(empty,camera,target,RHI::ResourceState::ShaderResource),"empty working set");Check(!renderer->m_impl->instanceBuffer && !renderer->m_impl->materialIndexBuffer && renderer->m_impl->mainSets.empty(),"empty scene releases working buffers and descriptor cache");
 }
 device.DestroyTexture(target);renderer.reset();device.Complete();Check(device.WaitIdle(),"drain teardown");
 Check(device.GetResourceAllocationCounters().buffers.live==0 && device.setsLive==0,"teardown must release buffers and descriptor owners");
 std::cout<<"PASS "<<mode<<'\n';return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
