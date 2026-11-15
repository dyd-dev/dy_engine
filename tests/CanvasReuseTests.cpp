#include <dyf/Renderer.h>
#include <dyf/Canvas.h>
#include <dyf/Scene.h>
#include <dyf/Camera.h>
#include <dyf/RHI.h>
#include "Backends/Null/NullDevice.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace RHI=dyf::RHI;
namespace {
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct Commands : RHI::ICommandList {
    std::vector<std::function<void()>> work;
    unsigned draws=0;
    ~Commands() override=default;
    void ResourceBarrierNative(const RHI::ResourceBarrierDesc*,uint32_t) override {}
    void GlobalBarrierNative() override {}
    void BeginRenderingNative(const RHI::RenderingDesc&) override {}
    void EndRenderingNative() override {}
    void BindGraphicsPipelineNative(RHI::PipelineHandle) override {}
    void BindResourceSetNative(RHI::ResourceSetHandle) override {}
    void BindVertexBufferNative(uint32_t,RHI::BufferHandle,uint32_t) override {}
    void BindIndexBufferNative(RHI::BufferHandle,RHI::Format,uint32_t) override {}
    void SetInlineConstantsNative(uint32_t,uint32_t,const void*) override {}
    void SetViewportNative(const RHI::Viewport&) override {}
    void SetScissorNative(const RHI::Rect&) override {}
    void SetStencilReferenceNative(uint32_t) override {}
    void DrawInstancedNative(uint32_t,uint32_t,uint32_t,uint32_t) override {++draws;}
    void DrawIndexedInstancedNative(uint32_t,uint32_t,uint32_t,int32_t,uint32_t) override {++draws;}
    bool CloseNative() override {return true;}
};
struct Device : dyf::Backends::NullDevice {
    uint64_t submitted=0,completed=0;
    unsigned textureUploads=0,vertexUploads=0,setsCreated=0,setsLive=0,draws=0;
    bool failSubmit=false,failDraw=false,failTextureUpload=false,failBufferUpload=false,failSet=false,delayed=false;
    std::vector<Commands*> pending;
    std::vector<float> uploadedX;
    std::vector<std::array<uint8_t,4>> pixels;
    Device(){Check(Initialize(nullptr,{3})==0,"device initialization");}
    ~Device(){Complete();ReleaseResources();}
    bool CreateSwapchainNative(const RHI::SwapchainDesc& d) override {auto copy=d;copy.initialWidth=copy.initialHeight=128;return NullDevice::CreateSwapchainNative(copy);}
    uint64_t GetCompletedSubmissionNative() override {return completed;}
    uint64_t GetLastSubmissionNative() const override {return submitted;}
    bool WaitIdleNative() override {Complete();return true;}
    RHI::ICommandList* AcquireCommandListNative() override {return new Commands;}
    void DiscardCommandListNative(RHI::ICommandList* c) override {delete static_cast<Commands*>(c);}
    void Apply(Commands* c){for(auto& work:c->work)work();draws+=c->draws;delete c;}
    void Complete(){for(auto* c:pending)Apply(c);pending.clear();completed=submitted;}
    bool SubmitNative(RHI::ICommandList** c,uint32_t count) override {
        if(failSubmit){failSubmit=false;return false;}
        if(failDraw)for(uint32_t i=0;i<count;++i)if(static_cast<Commands*>(c[i])->draws){failDraw=false;return false;}
        ++submitted;
        for(uint32_t i=0;i<count;++i){auto* list=static_cast<Commands*>(c[i]);if(delayed)pending.push_back(list);else Apply(list);}
        if(!delayed)completed=submitted;return true;
    }
    RHI::ResourceSetHandle CreateResourceSetNative(const RHI::ResourceSetDesc& d) override {
        if(failSet){failSet=false;throw std::runtime_error("injected set failure");}
        auto* result=NullDevice::CreateResourceSetNative(d);if(result){++setsCreated;++setsLive;}return result;
    }
    void DestroyResourceSetNative(RHI::ResourceSetHandle set) override {--setsLive;NullDevice::DestroyResourceSetNative(set);}
    bool UpdateBufferNative(RHI::ICommandList& c,RHI::BufferHandle b,uint32_t,const void* data,uint32_t size) override {
        if(failBufferUpload){failBufferUpload=false;return false;}
        if(b->GetDesc().stride==32 && size>=4){float x;std::memcpy(&x,data,4);static_cast<Commands&>(c).work.push_back([this,x]{++vertexUploads;uploadedX.push_back(x);});}
        return true;
    }
    bool UpdateTextureNative(RHI::ICommandList& c,RHI::TextureHandle,uint32_t,uint32_t,const void* data,uint32_t size,uint32_t,uint32_t) override {
        if(failTextureUpload){failTextureUpload=false;return false;}
        Check(size>=4,"texture data");std::array<uint8_t,4> color;std::memcpy(color.data(),data,4);
        static_cast<Commands&>(c).work.push_back([this,color]{++textureUploads;pixels.push_back(color);});return true;
    }
};
dyf::RendererShaderDesc Shaders(){static const uint8_t code[]={1};dyf::RendererShaderDesc s;
    for(auto* p:{&s.meshVertex,&s.shadowVertex,&s.canvasVertex,&s.toneMapVertex})*p={RHI::ShaderStage::Vertex,"main",code,1};
    for(auto* p:{&s.meshFragment,&s.canvasFragment,&s.toneMapFragment})*p={RHI::ShaderStage::Fragment,"main",code,1};return s;}
}
int main(int argc,char** argv){try {
    const std::string mode=argc>1?argv[1]:"reuse";
    Device device;dyf::RendererConfig config;config.enableProfilerHud=false;config.lighting.enabled=false;
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);Check(bool(renderer),"renderer");Check(renderer->SetShaders(Shaders()),"shaders");
    const dyf::Image red(1,1,{220,20,30,255});dyf::Canvas canvas(128,128);
    const auto fill=[&](unsigned frame,unsigned count=8,const dyf::Image& image=dyf::Image{}) {
        canvas.Clear();for(unsigned i=0;i<count;++i)canvas.Image(image.IsValid()?image:red,{float(frame)+i,20,8,8});
    };
    fill(1);Check(renderer->Render(canvas),"warm moving frame");
    const auto before=device.GetResourceAllocationCounters();const auto initialSets=device.setsCreated,initialUploads=device.textureUploads;
    if(mode=="reuse" || mode=="batch" || mode=="queued") {
        device.delayed=mode=="queued";const auto oldDraws=device.draws,oldUploads=device.vertexUploads;
        for(unsigned frame=2;frame<14;++frame){fill(frame);Check(renderer->Render(canvas),"moving frame");}
        device.Complete();const auto after=device.GetResourceAllocationCounters();
        Check(after.buffers.created==before.buffers.created,"moving frames must reuse vertex-buffer capacity");
        Check(after.textures.created==before.textures.created && device.setsCreated==initialSets,"moving images must reuse textures and sets");
        Check(device.textureUploads==initialUploads,"unchanged immutable images must not be uploaded again");
        Check(device.vertexUploads==oldUploads+12,"every moving frame must upload new vertices");
        Check(device.draws==oldDraws+12,"eight adjacent compatible quads must merge into one draw per frame");
        for(unsigned frame=2;frame<14;++frame)Check(device.uploadedX[device.uploadedX.size()-14+frame]==float(frame),"queued commands preserve independent moving bytes");
    } else if(mode=="boundaries") {
        canvas.Clear();canvas.Image(red,{2,2,8,8});canvas.Image(red,{3,3,8,8});canvas.SetClipRect({0,0,50,50});canvas.Image(red,{4,4,8,8});
        const dyf::Image blue(1,1,{10,20,230,255});canvas.Image(blue,{5,5,8,8});canvas.Image(red,{6,6,8,8});
        const auto draws=device.draws;Check(renderer->Render(canvas),"ordered image boundaries");Check(device.draws-draws==4,"coalescing must preserve clip and A/B/A image order");
    } else if(mode=="submit-retry" || mode=="submit-retry-same" || mode=="texture-retry" || mode=="buffer-retry" || mode=="set-retry") {
        const dyf::Image blue(1,1,{10,20,230,255});fill(10,mode=="submit-retry-same"?8:200,blue);
        if(mode=="submit-retry" || mode=="submit-retry-same")device.failSubmit=true;if(mode=="texture-retry")device.failTextureUpload=true;
        if(mode=="buffer-retry")device.failBufferUpload=true;if(mode=="set-retry")device.failSet=true;
        Check(!renderer->Render(canvas),"injected failure must be reported");device.Complete();
        const auto failed=device.GetResourceAllocationCounters();
        Check(failed.buffers.live==before.buffers.live && failed.textures.live==before.textures.live,"failed growth must roll back new resource owners");
        const auto uploads=device.textureUploads;Check(renderer->Render(canvas),"failed Canvas retries");Check(device.textureUploads>uploads,"failed new image upload must retry");
        Check(device.uploadedX.back()==10,"retry preserves current moving geometry");
    } else if(mode=="overlay-retry") {
        dyf::Scene scene;auto entity=scene.Add(dyf::CreateCubeMesh(.3f));Check(bool(entity),"overlay Scene");dyf::Camera camera;
        Check(renderer->Render(scene,camera,canvas),"Scene+Canvas warmup");const auto old=renderer->GetLastRenderCompletion();
        const auto stable=device.GetResourceAllocationCounters();const dyf::Image blue(1,1,{10,20,230,255});fill(10,200,blue);
        device.failDraw=true;Check(!renderer->Render(scene,camera,canvas),"Scene overlay submission failure");device.Complete();
        const auto failed=device.GetResourceAllocationCounters();Check(failed.buffers.live==stable.buffers.live && failed.textures.live==stable.textures.live,"failed overlay must roll back Canvas owners");
        const auto uploads=device.textureUploads;Check(entity.SetPosition({.1f,0,0}) && renderer->Render(scene,camera,canvas),"moving overlay retries");
        Check(renderer->GetLastRenderCompletion()!=old && device.textureUploads>uploads && device.uploadedX.back()==10,"overlay retry must submit missing image and moving vertices");
    } else if(mode=="empty") {
        canvas.Clear();device.failSubmit=true;Check(!renderer->Render(canvas),"empty submission failure");
        Check(renderer->Render(canvas),"empty retry");fill(3);Check(renderer->Render(canvas),"moving Canvas after empty frame");
        Check(device.GetResourceAllocationCounters().buffers.created==before.buffers.created,"empty Canvas must retain vertex capacity");
        Check(device.textureUploads==initialUploads && device.uploadedX.back()==3,"empty rollback must preserve cached images and next moving bytes");
    } else if(mode=="replacement") {
        auto linear=red;linear.SetColorSpace(dyf::ColorSpace::Linear);fill(2,8,linear);Check(renderer->Render(canvas),"color-space variant");
        Check(device.textureUploads==initialUploads+1,"same pixels with changed color space need distinct texture");
        dyf::Image blue(1,1,{10,20,230,255});fill(3,8,blue);blue=red;Check(renderer->Render(canvas),"retained Canvas image");
        Check(device.pixels.back()[2]==230,"caller Image reassignment must not replace recorded pixels");
    } else if(mode=="bounded-cache") {
        for(unsigned frame=2;frame<40;++frame){const dyf::Image unique(1,1,{uint8_t(frame),80,30,255});fill(frame,8,unique);Check(renderer->Render(canvas),"rotating images");}
        Check(device.GetResourceAllocationCounters().textures.live<=5,"expired Canvas images must not accumulate indefinitely");
    } else if(mode=="growth") {
        fill(2,100);Check(renderer->Render(canvas),"larger moving Canvas");const auto grown=device.GetResourceAllocationCounters().buffers.created;
        for(unsigned frame=3;frame<13;++frame){fill(frame,frame%2?90:100);Check(renderer->Render(canvas),"within-capacity vertex update");}
        Check(device.GetResourceAllocationCounters().buffers.created==grown,"smaller moving Canvas must retain vertex capacity");
    } else throw std::runtime_error("unknown mode");
    device.Complete();renderer.reset();Check(device.WaitIdle(),"teardown wait");const auto final=device.GetResourceAllocationCounters();
    Check(final.buffers.live==0 && final.textures.live==0 && final.pipelines.live==0 && device.setsLive==0,"Canvas teardown must release cached resources");
    std::cout<<"PASS Canvas "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
