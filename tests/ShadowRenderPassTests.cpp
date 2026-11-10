#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <thread>
#include <utility>
#include <vector>
#define private public
#include "dyf/Renderer.h"
#undef private
#include "dyf/RendererInternal.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/RHI.h"
#include "Backends/Null/NullDevice.h"

namespace {
namespace R=dyf::RHI;
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct Trace {
    std::vector<R::Rect> areas,scissors;
    uint32_t ends=0,barriers=0;
};
struct Commands final:R::ICommandList {
    Trace& trace;
    explicit Commands(Trace& value):trace(value) {}
    void ResourceBarrierNative(const R::ResourceBarrierDesc* barriers,uint32_t count) override {
        for(uint32_t i=0;i<count;++i)
            if(barriers[i].before==R::ResourceState::DepthWrite && barriers[i].after==R::ResourceState::DepthWrite)
                ++trace.barriers;
    }
    void GlobalBarrierNative() override {}
    void BeginRenderingNative(const R::RenderingDesc& desc) override {
        Check(desc.depthStencilAttachment && desc.depthStencilAttachment->depthLoadOp==R::LoadOp::Clear,
            "Every dirty region, including an empty caster region, must be cleared");
        trace.areas.push_back(desc.renderArea);
    }
    void EndRenderingNative() override {++trace.ends;}
    void BindGraphicsPipelineNative(R::PipelineHandle) override {}
    void BindResourceSetNative(R::ResourceSetHandle) override {}
    void BindVertexBufferNative(uint32_t,R::BufferHandle,uint32_t) override {}
    void BindIndexBufferNative(R::BufferHandle,R::Format,uint32_t) override {}
    void SetInlineConstantsNative(uint32_t,uint32_t,const void*) override {}
    void SetViewportNative(const R::Viewport&) override {}
    void SetScissorNative(const R::Rect& rect) override {trace.scissors.push_back(rect);}
    void SetStencilReferenceNative(uint32_t) override {}
    void DrawInstancedNative(uint32_t,uint32_t,uint32_t,uint32_t) override {}
    void DrawIndexedInstancedNative(uint32_t,uint32_t,uint32_t,int32_t,uint32_t) override {}
    bool CloseNative() override {return true;}
};
struct Device final:dyf::Backends::NullDevice {
    Trace trace;
    bool renderArea;
    explicit Device(bool partial):renderArea(partial) {Check(Initialize(nullptr,{})==0,"Device initialization");}
    bool SupportsNative(R::Feature feature) const override {
        return feature==R::Feature::RenderArea ? renderArea : NullDevice::SupportsNative(feature);
    }
    bool CreateSwapchainNative(const R::SwapchainDesc& input) override {
        auto desc=input;desc.initialWidth=desc.initialHeight=64;return NullDevice::CreateSwapchainNative(desc);
    }
    R::ICommandList* AcquireCommandListNative() override {return new Commands(trace);}
    void DiscardCommandListNative(R::ICommandList* commands) override {delete static_cast<Commands*>(commands);}
};
void ClusterGeometry() {
    using Impl=dyf::Renderer::Impl;
    std::vector<Impl::CompactShadowVertex> vertices={
        {0,0,0,0,0,.25f},{1,0,0,1,0,.5f},{0,1,0,0,1,1},
        {100,0,0,0,0,.25f},{101,0,0,1,0,.5f},{100,1,0,0,1,1}};
    std::vector<uint32_t> indices;
    for(uint32_t i=0;i<1024;++i)indices.insert(indices.end(),{0,1,2,3,4,5});
    const auto original=indices;
    std::vector<Impl::ShadowCluster> clusters;
    Impl::BuildShadowClusters(vertices,indices,clusters);
    Check(clusters.size()>1,"Distant triangle groups still share one shadow culling bound");
    std::array<uint32_t,2> triangles={};uint32_t next=0;
    for(const auto& cluster:clusters) {
        Check(cluster.firstIndex==next && cluster.indexCount%3==0 && cluster.bounds.valid,"Incomplete shadow cluster partition");
        Check(cluster.bounds.max.x-cluster.bounds.min.x<2,"Spatial split mixed distant islands");
        for(uint32_t i=cluster.firstIndex;i<cluster.firstIndex+cluster.indexCount;i+=3) {
            const auto a=indices[i];Check((a==0 || a==3) && indices[i+1]==a+1 && indices[i+2]==a+2,"Triangle winding or corners changed");
            ++triangles[a/3];
            for(uint32_t corner=0;corner<3;++corner) {
                const auto& v=vertices[indices[i+corner]];
                Check(v.px>=cluster.bounds.min.x && v.px<=cluster.bounds.max.x &&
                    v.py>=cluster.bounds.min.y && v.py<=cluster.bounds.max.y &&
                    v.pz>=cluster.bounds.min.z && v.pz<=cluster.bounds.max.z,"Cluster bound excludes a triangle vertex");
            }
        }
        next+=cluster.indexCount;
    }
    Check(next==indices.size() && triangles[0]==1024 && triangles[1]==1024,"Shadow split lost or duplicated triangles");
    indices={0,1,2};Impl::BuildShadowClusters(vertices,indices,clusters);
    Check(clusters.empty() && indices==std::vector<uint32_t>({0,1,2}),"Small mesh was split");
    auto tiny=vertices;
    for(auto& v:tiny) {v.px*=1e-38f;v.py*=1e-38f;v.pz*=1e-38f;}
    indices=original;Impl::BuildShadowClusters(tiny,indices,clusters);
    Check(clusters.empty() && indices==original,"Tiny triangle centroids entered overflowing spatial quantization");
    for(uint32_t i=0;i<tiny.size();++i) {tiny[i].px=float(i%3)-1; tiny[i].py=0; tiny[i].pz=i<3?0:1e-37f;}
    Impl::BuildShadowClusters(tiny,indices,clusters);
    Check(clusters.empty() && indices==original,"Large vertex bounds hid an overflowing centroid quantization range");
    indices=original;vertices[0].px=std::numeric_limits<float>::quiet_NaN();
    Impl::BuildShadowClusters(vertices,indices,clusters);
    Check(clusters.empty() && indices==original,"Invalid geometry lost its original fallback");
}
bool Same(const R::Rect& rect,int32_t x,int32_t y,uint32_t width,uint32_t height) {
    return rect.x==x && rect.y==y && rect.width==width && rect.height==height;
}
void Run(bool supportsArea,const std::array<bool,4>& dirty,uint32_t expectedPasses) {
    Device device(supportsArea);
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),{});
    Check(bool(renderer),"Renderer initialization");
    auto& impl=*renderer->m_impl;
    R::TextureDesc depth;depth.width=depth.height=64;depth.format=R::Format::D32_FLOAT;
    depth.usage=R::TextureUsage::DepthStencil|R::TextureUsage::ShaderResource;
    impl.shadowDepthTarget=device.CreateTexture(depth);
    Check(impl.shadowDepthTarget!=nullptr,"Depth atlas creation");
    dyf::Renderer::Impl::ShadowData shadows;
    shadows.viewCount=4;shadows.columns=shadows.rows=2;shadows.resolution=32;
    std::copy(dirty.begin(),dirty.end(),shadows.dirtyViews.begin());
    dyf::Scene scene;dyf::Camera camera;
    auto* commands=device.AcquireCommandList();Check(commands!=nullptr,"Command list creation");
    const R::ResourceBarrierDesc transition={nullptr,impl.shadowDepthTarget,R::ResourceState::Undefined,R::ResourceState::DepthWrite,{}};
    commands->ResourceBarrier(&transition,1);
    Check(impl.RecordShadowPass(scene,camera,shadows,*commands,nullptr) && commands->Close(),"Shadow pass recording");
    Check(device.PrepareCommandLists(&commands,1,1),"Shadow pass native replay");
    device.DestroyCommandList(commands);
    const auto& trace=device.trace;
    Check(trace.areas.size()==expectedPasses && trace.ends==expectedPasses,"All dirty shadow tiles must share one render pass");
    const bool all=std::all_of(dirty.begin(),dirty.end(),[](bool value){return value;});
    if(all) {
        Check(Same(trace.areas[0],0,0,64,64),"Full update must clear the entire atlas once");
        Check(trace.barriers==0,"Full update must not serialize disjoint tiles with depth barriers");
    } else {
        Check(trace.barriers==(expectedPasses?expectedPasses-1:0),"Partial update barrier count changed");
    }
    size_t index=0;
    for(uint32_t view=0;view<4;++view)if(dirty[view]) {
        const int32_t x=int32_t(view%2)*32,y=int32_t(view/2)*32;
        Check(index<trace.scissors.size() && Same(trace.scissors[index],x,y,32,32),"Dirty tile scissor changed");
        if(!all)Check(index<trace.areas.size() && Same(trace.areas[index],x,y,32,32),"Partial clear touched a cached tile");
        ++index;
    }
    Check(index==trace.scissors.size(),"Clean tile received shadow commands");
}
}
int main() try {
    ClusterGeometry();
    Run(true,{false,true,false,true},2);
    Run(true,{false,false,false,false},0);
    Run(false,{true,true,true,true},1);
    Run(true,{true,true,true,true},1);
    std::puts("PASS shadow render passes: full update batching, partial cache preservation and fallback");
    return 0;
} catch(const std::exception& error) {std::fprintf(stderr,"FAIL shadow render passes: %s\n",error.what());return 1;}
