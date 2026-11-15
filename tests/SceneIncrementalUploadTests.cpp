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
#include "dyf/ShaderLayout.h"
#include "dyf/GpuVisibility.h"
#include <stdexcept>
#include <iostream>
namespace {
namespace RHI=dyf::RHI;
using dyf::Math::float4x4;
unsigned drawCount=0,instanceCount=0;
void Check(bool v,const char* m) { if(!v) throw std::runtime_error(m); }
struct Commands : RHI::ICommandList {
    bool hasDraw=false;
 struct Upload {RHI::BufferHandle buffer;uint32_t offset;std::vector<uint8_t> bytes;};
 std::vector<Upload> uploads;
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
    void DrawIndexedIndirectNative(RHI::BufferHandle,uint32_t,uint32_t,uint32_t) override {hasDraw=true;}
    void ResetTimestampsNative(RHI::TimestampQueryHandle, uint32_t, uint32_t) override {}
    void WriteTimestampNative(RHI::TimestampQueryHandle, uint32_t) override {}
    bool CloseNative() override { return true; }
};

struct Device : dyf::Backends::NullDevice {
 struct ComputePipeline : RHI::Pipeline {explicit ComputePipeline(const RHI::PipelineLayoutDesc& layout):Pipeline(layout,true) {} ~ComputePipeline() override=default;};
 unsigned buffers=0,sets=0,setsLive=0;
 uint64_t completed=0,submitted=0;
 unsigned recordCountdown=0;
 bool delayed=false,failDraw=false,failUpload=false,throwSet=false,bindless=false,failRecord=false,compute=false;
 std::vector<Commands::Upload> recorded;
 std::map<RHI::BufferHandle,std::vector<uint8_t>> contents;
 bool SupportsNative(RHI::Feature feature) const override {return feature==RHI::Feature::Compute ? compute : feature==RHI::Feature::DescriptorIndexing ? bindless : NullDevice::SupportsNative(feature);}
 RHI::PipelineHandle CreateComputePipelineNative(const RHI::ComputePipelineDesc& desc) override {return new ComputePipeline(desc.layout);}
 void DestroyPipelineNative(RHI::PipelineHandle pipeline) override {if(auto* computePipeline=dynamic_cast<ComputePipeline*>(pipeline))delete computePipeline;else NullDevice::DestroyPipelineNative(pipeline);}
 std::vector<RHI::ICommandList*> pending;
 ~Device() {Complete();ReleaseResources();}
 void Complete() {completed=submitted;for(auto* c:pending){Apply(static_cast<Commands*>(c));DiscardCommandListNative(c);}pending.clear();}
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
  for(uint32_t i=0;i<n;++i) {if(delayed)pending.push_back(c[i]);else {Apply(static_cast<Commands*>(c[i]));DiscardCommandListNative(c[i]);}}
  if(!delayed)completed=submitted;
  return true;
 }
 void Apply(Commands* c) {for(const auto& u:c->uploads){auto& b=contents[u.buffer];b.resize(u.buffer->GetDesc().size);std::copy(u.bytes.begin(),u.bytes.end(),b.begin()+u.offset);}}
 bool UpdateBufferNative(RHI::ICommandList& c,RHI::BufferHandle b,uint32_t offset,const void* data,uint32_t size) override {
  if(failRecord){failRecord=false;return false;}
  if(recordCountdown && --recordCountdown==0)return false;
  const auto* bytes=static_cast<const uint8_t*>(data);Commands::Upload u{b,offset,{bytes,bytes+size}};
  recorded.push_back(u);static_cast<Commands&>(c).uploads.push_back(std::move(u));return true;
 }
 std::vector<Commands::Upload> For(RHI::BufferHandle b) const {std::vector<Commands::Upload> result;for(const auto& u:recorded)if(u.buffer==b)result.push_back(u);return result;}
 float X(RHI::BufferHandle b,uint32_t entity) const {float x;std::memcpy(&x,contents.at(b).data()+entity*b->GetDesc().stride+48,4);return x;}
 RHI::BufferHandle CreateBufferNative(const RHI::BufferDesc& d) override {++buffers;return NullDevice::CreateBufferNative(d);}
 RHI::ResourceSetHandle CreateResourceSetNative(const RHI::ResourceSetDesc& d) override {++sets;if(throwSet)throw std::runtime_error("injected set failure");auto* set=NullDevice::CreateResourceSetNative(d);if(set)++setsLive;return set;}
 void DestroyResourceSetNative(RHI::ResourceSetHandle set) override {--setsLive;NullDevice::DestroyResourceSetNative(set);}
};

}
int main(int argc,char** argv) {
 try {
 const std::string mode=argc>1?argv[1]:"static";
 Device device;device.bindless=mode=="bindless" || mode=="bindless-retry" || mode=="material-cache";
 dyf::RendererConfig config;config.enableProfilerHud=false;config.enableFrustumCulling=false;
 auto makeRenderer=[&] {auto r=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);Check(bool(r),"renderer");static const uint8_t code[]={1};r->m_impl->vertexShader=device.CreateShader({RHI::ShaderStage::Vertex,"main",code,1});r->m_impl->fragmentShader=device.CreateShader({RHI::ShaderStage::Fragment,"main",code,1});return r;};
 auto renderer=makeRenderer();
 dyf::MeshData mesh;mesh.vertices.resize(3);mesh.indices={0,1,2};mesh.vertices[1].position.x=.2f;mesh.vertices[2].position.y=.2f;
 const auto fill=[&](dyf::Scene& s,float x=0){auto transform=float4x4::Identity();transform.m[12]=x;auto e=s.Add(mesh,{},transform);for(unsigned i=1;i<1000;++i)Check(bool(s.AddInstance(e)),"add instance");return e;};
 dyf::Scene scene;auto source=fill(scene);dyf::Camera camera;
 auto* target=device.CreateTexture({16,16,1,1,RHI::Format::R8G8B8A8_UNORM,RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource});
 if(mode.rfind("gpu-",0)==0) {
  device.compute=true;
  renderer->m_impl->config.enableGpuCulling=true;
  RHI::BufferHandle inputs=nullptr;
  bool targetReady=false;
  const auto gpuRender=[&](dyf::Renderer& r,const dyf::Scene& s,const std::vector<dyf::RendererDrawDesc>* draws=nullptr) {
   device.recorded.clear();const bool result=r.m_impl->RenderToTexture(s,camera,target,targetReady?RHI::ResourceState::ShaderResource:RHI::ResourceState::Undefined,draws);
   targetReady=targetReady || result;
   if(result)for(const auto& upload:device.recorded)
    if(upload.buffer->GetDesc().stride==sizeof(dyf::GpuVisibility::Instance) &&
       upload.bytes.size()==s.GetEntityCount()*sizeof(dyf::GpuVisibility::Instance))inputs=upload.buffer;
   return result;
  };
  Check(gpuRender(*renderer,scene),"GPU warmup");
  Check(renderer->m_impl->visibilityActive,"GPU path active");
  Check(inputs!=nullptr,"GPU visibility input upload");
  const auto inputAt=[&](unsigned index) {dyf::GpuVisibility::Instance result;std::memcpy(&result,device.contents.at(inputs).data()+index*sizeof(result),sizeof(result));return result;};
  if(mode=="gpu-camera") {
   for(float value:inputAt(0).previousMVP.m)Check(value==0,"motion-disabled visibility inputs must omit previous MVP");
   for(unsigned frame=0;frame<3;++frame) {
    Check(camera.LookAt({float(frame+1),0,2},{0,0,0},{0,1,0}),"camera move");
    Check(gpuRender(*renderer,scene),"camera-only GPU frame");
    Check(device.For(inputs).empty(),"camera-only frame must upload zero visibility instance bytes");
    Check(renderer->m_impl->visibilityDraws.size()==1,"static equivalent materials keep one GPU batch");
   }
  }
  if(mode=="gpu-mutations") {
   Check(renderer->m_impl->visibilityDraws.size()==1,"equivalent materials canonicalize");
   auto material=source.GetMaterial();material.roughnessFactor=.25f;Check(source.SetMaterial(material),"material edit");
   Check(gpuRender(*renderer,scene) && renderer->m_impl->visibilityDraws.size()==2,"material edit splits batch");
   auto second=scene.GetEntity(static_cast<dyf::EntityID>(1));auto lighting=second.GetLighting();lighting.receiveShadow=false;Check(second.SetLighting(lighting),"lighting edit");
   Check(gpuRender(*renderer,scene) && renderer->m_impl->visibilityDraws.size()==3,"lighting edit splits batch");
   auto third=scene.GetEntity(static_cast<dyf::EntityID>(2));auto world=third.GetTransform();world.m[0]=-2;world.m[5]=3;world.m[12]=7;Check(third.SetTransform(world),"mirrored nonuniform transform");
   Check(gpuRender(*renderer,scene) && renderer->m_impl->visibilityDraws.size()==4,"mirror edit regroups batch");
   Check(inputAt(2).model.m[12]==7 && inputAt(2).centerRadius.x>6 && inputAt(2).centerRadius.w>.3f,"updated conservative world bounds");
   Check(bool(scene.AddInstance(source)),"append GPU instance");Check(gpuRender(*renderer,scene),"append GPU frame");
   bool full=false;for(const auto& upload:device.recorded)full=full || (upload.buffer->GetDesc().stride==sizeof(dyf::GpuVisibility::Instance) && upload.bytes.size()==1001*sizeof(dyf::GpuVisibility::Instance));
   Check(full,"append rebuilds complete GPU inputs");
   std::vector<dyf::RendererDrawDesc> draws(scene.GetEntityCount());draws[0].vertexBuffer=renderer->m_impl->m_meshes[0].vertexBuffer;draws[0].hasLocalBounds=true;draws[0].localBounds={{-4,-4,-4},{4,4,4},true};
   Check(gpuRender(*renderer,scene,&draws),"custom bounds frame");
   Check(renderer->m_impl->visibilityDraws.size()>=4,"explicit geometry retains compatible batches");
   Check(gpuRender(*renderer,scene),"restore stock geometry");
  }
  if(mode=="gpu-owners") {
   dyf::Scene other;fill(other,9);Check(gpuRender(*renderer,other),"other Scene frame");Check(device.X(inputs,0)==9,"different Scene cannot reuse GPU transforms");
   auto second=makeRenderer();second->m_impl->config.enableGpuCulling=true;Check(gpuRender(*second,scene),"second renderer warmup");
   Check(source.SetPosition({8,0,0}),"shared Scene edit");Check(gpuRender(*renderer,scene) && device.X(inputs,0)==8,"first owner updates");Check(gpuRender(*second,scene),"second owner updates independently");
   alignas(dyf::Scene) unsigned char storage[sizeof(dyf::Scene)];auto* reused=new(storage)dyf::Scene;fill(*reused,2);Check(gpuRender(*renderer,*reused),"ABA first Scene");reused->~Scene();reused=new(storage)dyf::Scene;fill(*reused,6);Check(gpuRender(*renderer,*reused) && device.X(inputs,0)==6,"same-address new Scene invalidates GPU cache");reused->~Scene();
  }
  if(mode=="gpu-retry") {
   const auto submittedBytes=device.contents.at(inputs);
   Check(source.SetPosition({6,0,0}),"GPU transform edit");device.failUpload=true;Check(!gpuRender(*renderer,scene),"failed GPU submission");Check(device.contents.at(inputs)==submittedBytes,"failure keeps submitted GPU bytes");
   Check(gpuRender(*renderer,scene),"retry GPU frame");
   const auto retries=device.For(inputs);
   float submittedX=0;std::memcpy(&submittedX,device.contents.at(inputs).data()+48,sizeof(submittedX));
   Check(retries.size()==1,"retry records dirty GPU instance upload");
   Check(submittedX==6,"retry submits updated GPU instance bytes");
   Check(gpuRender(*renderer,scene) && device.For(inputs).empty(),"successful retry stabilizes");
  }
  if(mode=="gpu-motion") {
   Check(camera.LookAt({1,0,2},{0,0,0},{0,1,0}) && camera.SetOrthographic(2,2,.1f,10),"motion camera");
   const auto expected=camera.projection*camera.view*source.GetTransform();
   for(bool historyValid:{true,false}) {
    std::vector<dyf::RendererDrawDesc> draws(scene.GetEntityCount());draws[0].deformationHistoryValid=historyValid;
    auto* commands=device.AcquireCommandList();Check(commands!=nullptr,"motion recording");device.recorded.clear();
    Check(renderer->m_impl->PrepareVisibility(scene,camera,*commands,16,16,&draws,true),"requested motion visibility");
    Check(commands->Close() && device.PrepareCommandLists(&commands,1,1),"motion native recording");
    bool inspected=false;
    for(const auto& upload:device.recorded)if(upload.buffer->GetDesc().stride==sizeof(dyf::GpuVisibility::Instance)) {
     dyf::GpuVisibility::Instance value;std::memcpy(&value,upload.bytes.data(),sizeof(value));
     for(unsigned i=0;i<16;++i)Check(value.previousMVP.m[i]==(historyValid?expected.m[i]:0.f),"motion request preserves previous MVP and invalid deformation history");inspected=true;
    }
    Check(inspected,"motion input upload");device.DestroyCommandList(commands);
   }
  }
  device.Complete();device.DestroyTexture(target);renderer.reset();Check(device.WaitIdle(),"GPU teardown");Check(device.GetResourceAllocationCounters().buffers.live==0,"GPU cache has no buffer ownership leak");std::cout<<"PASS "<<mode<<'\n';return 0;
 }
 // Explicit draw inputs keep this test on the shared incremental stream rather than the compact CPU batch stream.
 const auto render=[&](dyf::Renderer& r,const dyf::Scene& s,RHI::ResourceState before=RHI::ResourceState::ShaderResource){device.recorded.clear();std::vector<dyf::RendererDrawDesc> draws(s.GetEntityCount());return r.m_impl->RenderToTexture(s,camera,target,before,&draws);};
 const auto move=[&](unsigned i,float x){Check(scene.GetEntity(static_cast<dyf::EntityID>(i)).SetPosition({x,0,0}),"move entity");};
 Check(render(*renderer,scene,RHI::ResourceState::Undefined),"warmup");auto* instances=renderer->m_impl->instanceBuffer;
 const auto stride=2*sizeof(float4x4);
 Check(instances && instances->GetDesc().stride==stride && device.For(instances).size()==1 && device.For(instances)[0].bytes.size()==1000*stride,"initial full upload");
 if(mode=="static" || mode=="same-value") {
  if(mode=="same-value")Check(source.SetTransform(source.GetTransform()) && source.SetPosition({0,0,0}),"equal setters");
  Check(render(*renderer,scene),"static frame");Check(device.For(instances).empty(),"static and same-value scene must upload zero transform bytes");Check(device.For(renderer->m_impl->lightingBuffer).empty(),"static lighting must upload zero bytes");
 }
 if(mode=="sparse" || mode=="submit-retry" || mode=="record-retry" || mode=="second-range-retry") {
  move(3,3);move(4,4);move(500,5);
  if(mode!="sparse"){if(mode=="submit-retry")device.failUpload=true;else if(mode=="second-range-retry")device.recordCountdown=2;else device.failRecord=true;Check(!render(*renderer,scene),"injected upload failure");Check(device.X(instances,3)==0,"failed upload must not publish GPU bytes");}
  Check(render(*renderer,scene),"changed frame or retry");auto u=device.For(instances);Check(u.size()==2 && u[0].offset==3*stride && u[0].bytes.size()==2*stride && u[1].offset==500*stride && u[1].bytes.size()==stride,"sparse changes must coalesce adjacent transforms and preserve offsets");Check(device.X(instances,3)==3 && device.X(instances,4)==4 && device.X(instances,500)==5 && device.X(instances,2)==0,"patched transforms preserve untouched bytes");Check(render(*renderer,scene) && device.For(instances).empty(),"successful retry commits dirty cache");
 }
 if(mode=="all-moving" || mode=="fragmented") {
  const unsigned frames=mode=="all-moving"?120:1;
  for(unsigned frame=0;frame<frames;++frame) {
   for(unsigned i=0;i<1000;i+=(mode=="fragmented"?2:1))move(i,float(i+1)+float(frame)*.125f);
   Check(render(*renderer,scene),"animated frame");auto u=device.For(instances);
   if(mode=="all-moving")Check(u.size()==1 && u[0].offset==0 && u[0].bytes.size()==1000*stride,"adjacent animation must coalesce into one full upload");
   else {Check(u.size()==500,"disjoint transforms retain separate upload spans");for(size_t i=0;i<u.size();++i)Check(u[i].offset==i*2*stride && u[i].bytes.size()==stride,"fragmented upload span");}
   for(unsigned i=0;i<1000;i+=(mode=="fragmented"?2:1))Check(device.X(instances,i)==float(i+1)+float(frame)*.125f,"every animated transform reaches GPU buffer");
  }
  std::cout<<"motion frames="<<frames<<" moving_entities="<<(mode=="all-moving"?1000:500)<<" transform_bytes_per_frame="<<(mode=="all-moving"?1000:500)*stride<<'\n';
 }
 if(mode=="growth") {Check(bool(scene.AddInstance(source)),"grow");Check(render(*renderer,scene),"growth frame");instances=renderer->m_impl->instanceBuffer;auto u=device.For(instances);Check(u.size()==1 && u[0].bytes.size()==1001*stride,"growth needs complete replacement upload");Check(render(*renderer,scene) && device.For(instances).empty(),"grown scene stabilizes");}
 if(mode=="replacement") {dyf::Scene other;Check(bool(fill(other,9)),"replacement transform");Check(render(*renderer,other),"replacement frame");auto u=device.For(instances);Check(u.size()==1 && u[0].bytes.size()==1000*stride && device.X(instances,0)==9,"equal-count different scene requires full upload");}
 if(mode=="aba") {alignas(dyf::Scene) unsigned char storage[sizeof(dyf::Scene)];auto* other=new(storage)dyf::Scene;Check(bool(fill(*other)),"ABA first fill");Check(render(*renderer,*other),"ABA first scene");other->~Scene();other=new(storage)dyf::Scene;Check(bool(fill(*other)),"ABA second fill");Check(other->GetEntity(static_cast<dyf::EntityID>(7)).SetPosition({7,0,0}),"ABA second move");Check(render(*renderer,*other),"ABA replacement");auto u=device.For(instances);Check(u.size()==1 && u[0].bytes.size()==1000*stride && device.X(instances,7)==7,"same-address Scene lifetime must force full upload");other->~Scene();}
 if(mode=="two-renderers") {auto second=makeRenderer();Check(render(*second,scene),"second warmup");move(0,8);Check(render(*renderer,scene),"first incremental");Check(device.For(instances).size()==1 && device.For(instances)[0].bytes.size()==stride,"first renderer patch");Check(render(*second,scene),"second incremental");Check(device.For(second->m_impl->instanceBuffer).size()==1 && device.For(second->m_impl->instanceBuffer)[0].bytes.size()==stride && device.X(second->m_impl->instanceBuffer,0)==8,"each Renderer consumes changes independently");}
 if(mode=="queued") {device.delayed=true;for(unsigned i=1;i<5;++i){move(0,float(i));Check(render(*renderer,scene),"queued frame");auto u=device.For(instances);Check(u.size()==1 && u[0].bytes.size()==stride,"queued patch span");float x;std::memcpy(&x,u[0].bytes.data()+48,4);Check(x==float(i),"command captures frame bytes immediately");}Check(device.X(instances,0)==0,"queued copies not executed");device.Complete();Check(device.X(instances,0)==4,"queued copies preserve submission order");}
 if(mode=="bindless") {auto* index=renderer->m_impl->materialIndexBuffer;Check(index!=nullptr,"material indices");Check(render(*renderer,scene),"bindless static");Check(device.For(index).empty(),"unchanged material indices skip upload");move(0,2);Check(render(*renderer,scene) && device.For(index).empty(),"transform edit does not upload indices");}
 if(mode=="material-cache") {
  auto* index=renderer->m_impl->materialIndexBuffer;Check(index && !renderer->m_impl->materialPages.empty(),"material page fixture");
  const auto* textures=renderer->m_impl->materialPages[0].textures.data();const auto* members=renderer->m_impl->materialPages[0].materials.data();
  Check(camera.LookAt({0,0,2},{0,0,0},{0,1,0}),"page camera move");Check(render(*renderer,scene),"static material pages");
  Check(device.For(index).empty() && textures==renderer->m_impl->materialPages[0].textures.data() && members==renderer->m_impl->materialPages[0].materials.data(),"camera-only material pages retain storage and uploaded indices");
  auto material=source.GetMaterial();material.baseColorTexture=dyf::Image(1,1,{20,40,60,255});Check(source.SetMaterial(material),"page texture edit");
  Check(render(*renderer,scene) && device.For(index).size()==1,"texture edit rebuilds material mapping");
  Check(render(*renderer,scene) && device.For(index).empty(),"updated pages stabilize");
  Check(bool(scene.AddInstance(source)),"page append");Check(render(*renderer,scene),"appended material page");
  Check(renderer->m_impl->materialIndexScratch.size()==scene.Materials().size()*8,"append extends material indices");
  dyf::Scene replacement;fill(replacement);Check(render(*renderer,replacement),"page owner replacement");
  Check(renderer->m_impl->materialPages[0].textures.size()==3 && renderer->m_impl->materialIndexScratch.size()==8000,"replacement drops stale texture mapping");
 }
 if(mode=="lighting-retry") {Check(camera.LookAt({0,0,2},{0,0,0},{0,1,0}),"camera move");device.failUpload=true;Check(!render(*renderer,scene),"lighting submit failure");Check(render(*renderer,scene),"lighting retry");Check(device.For(renderer->m_impl->lightingBuffer).size()==1,"failed lighting upload retries");Check(render(*renderer,scene) && device.For(renderer->m_impl->lightingBuffer).empty(),"lighting cache commits after successful Submit");}
 if(mode=="draw-retry") {move(0,6);device.failDraw=true;Check(!render(*renderer,scene),"combined upload and draw submission fails");Check(device.X(instances,0)==0,"rejected submission must not publish transform bytes");Check(render(*renderer,scene) && device.For(instances).size()==1 && device.X(instances,0)==6,"draw retry must resubmit uncommitted transforms");Check(render(*renderer,scene) && device.For(instances).empty(),"accepted retry commits the transform cache");}
 if(mode=="growth-retry") {Check(bool(scene.AddInstance(source)),"growth");device.failUpload=true;Check(!render(*renderer,scene),"failed replacement upload");Check(renderer->m_impl->instanceBuffer==instances,"failed upload preserves old buffer owner");Check(render(*renderer,scene),"replacement retry");instances=renderer->m_impl->instanceBuffer;auto u=device.For(instances);Check(u.size()==1 && u[0].bytes.size()==1001*stride,"replacement retry uploads complete data");}
 if(mode=="bindless-retry") {auto* index=renderer->m_impl->materialIndexBuffer;auto material=source.GetMaterial();material.baseColorTexture=dyf::Image(1,1,{255,0,0,255});Check(source.SetMaterial(material),"change index layout");device.failDraw=true;Check(!render(*renderer,scene),"material Submit failure");Check(render(*renderer,scene) && device.For(index).size()==1,"material index cache must retry missing submitted bytes");uint32_t normalIndex;std::memcpy(&normalIndex,device.contents.at(index).data()+4,4);Check(normalIndex==1,"new first-material texture moves shared white slot to index1");Check(render(*renderer,scene) && device.For(index).empty(),"material index retry then stabilizes");}
 if(mode=="scratch") {auto* transforms=renderer->m_impl->instanceScratch.data();auto* sets=renderer->m_impl->mainSetScratch.data();auto* bindings=renderer->m_impl->mainBindingScratch.data();for(unsigned i=0;i<5;++i){move(3,float(i+1));Check(render(*renderer,scene),"warm scratch frame");Check(renderer->m_impl->instanceScratch.data()==transforms && renderer->m_impl->mainSetScratch.data()==sets && renderer->m_impl->mainBindingScratch.data()==bindings,"warm transform, set, binding arrays reuse storage");}}
 if(mode=="shadow-static") {renderer->m_impl->config.lighting.shadows=true;renderer->m_impl->config.lighting.shadowQuality=dyf::ShadowQuality::Low;static const uint8_t code[]={1};renderer->m_impl->shadowVertexShader=device.CreateShader({RHI::ShaderStage::Vertex,"main",code,1});renderer->m_impl->shadowFragmentShader=device.CreateShader({RHI::ShaderStage::Fragment,"main",code,1});device.DestroyPipeline(renderer->m_impl->pipeline);renderer->m_impl->pipeline=nullptr;dyf::DirectionalLight light;light.castShadow=true;Check(bool(scene.Add(light)),"shadow light");Check(render(*renderer,scene),"shadow warmup");auto* shadow=renderer->m_impl->shadowMatrixBuffer;Check(shadow && device.For(shadow).size()==1,"initial shadow constants upload");Check(render(*renderer,scene) && device.For(shadow).empty(),"static shadow constants skip upload");Check(renderer->m_impl->mainBindingScratch.data()!=renderer->m_impl->shadowBindingScratch.data(),"parallel pass binding scratch has independent owners");}
 if(mode=="light-order") {const int priorities[]={0,10,50,10,10,100};for(unsigned i=0;i<6;++i){dyf::DirectionalLight light;light.priority=priorities[i];light.intensity=float(i+1);light.enabled=i!=5;Check(bool(scene.Add(light)),"light fixture");}Check(render(*renderer,scene),"light selection");dyf::RendererLightingConstants constants;std::memcpy(&constants,device.contents.at(renderer->m_impl->lightingBuffer).data(),sizeof(constants));Check(constants.directionalLightColor.w==3 && constants.directionalLights[0].directionIntensity.w==3 && constants.directionalLights[1].directionIntensity.w==2 && constants.directionalLights[2].directionIntensity.w==4 && constants.directionalLights[3].directionIntensity.w==5,"reused lighting selection preserves priority, stable ties, disabled filter and capacity");auto* indices=renderer->m_impl->lightingIndexScratch[0].data();Check(render(*renderer,scene) && renderer->m_impl->lightingIndexScratch[0].data()==indices && device.For(renderer->m_impl->lightingBuffer).empty(),"lighting index storage is reused and identical constants skip upload");}
 device.Complete();device.DestroyTexture(target);renderer.reset();Check(device.WaitIdle(),"teardown");Check(device.GetResourceAllocationCounters().buffers.live==0,"no buffer ownership leak");std::cout<<"PASS "<<mode<<'\n';return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
