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
#include "dyf/ImageDds.h"
#include "dyf/Scene.h"
#include "dyf/RHI.h"
#include "Backends/Null/NullDevice.h"

namespace {
void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct Device : dyf::Backends::NullDevice {
    Device() {Check(Initialize(nullptr,{})==0,"Null fixture initialization failed");}
    bool CreateSwapchainNative(const dyf::RHI::SwapchainDesc& input) override {
        auto desc=input;desc.initialWidth=128;desc.initialHeight=64;
        return NullDevice::CreateSwapchainNative(desc);
    }
};
}

int main() try
{
    using namespace dyf;
    namespace R=dyf::RHI;
    Device device;
    R::ResourceScope resources(device);
    RendererConfig config;config.lighting.reuseStaticShadows=true;config.enableFrustumCulling=true;
    auto renderer=Renderer::Create(device,reinterpret_cast<const void*>(1),config);
    Check(bool(renderer),"Null renderer fixture creation failed");
    auto& impl=*renderer->m_impl;impl.stockDrawInputs=true;
    const uint32_t code=0;
    auto* shader=resources.Keep(device.CreateShader({R::ShaderStage::Vertex,"main",&code,sizeof(code)}));
    Check(shader!=nullptr,"Shadow fixture shader failed");
    R::GraphicsPipelineDesc pipeline;pipeline.vertexShader=shader;pipeline.topology=R::PrimitiveTopology::TriangleList;
    pipeline.raster.fillMode=R::FillMode::Solid;pipeline.raster.cullMode=R::CullMode::None;
    pipeline.raster.frontFace=R::FrontFace::CounterClockwise;
    pipeline.depthStencil.format=R::Format::D32_FLOAT;
    pipeline.depthStencil.depthTestEnabled=true;pipeline.depthStencil.depthWriteEnabled=true;
    pipeline.depthStencil.depthCompareOp=R::CompareOp::LessEqual;
    impl.shadowPipeline=device.CreateGraphicsPipeline(pipeline);
    R::TextureDesc depth;depth.width=128;depth.height=64;depth.format=R::Format::D32_FLOAT;
    depth.usage=R::TextureUsage::DepthStencil|R::TextureUsage::ShaderResource;
    impl.shadowDepthTarget=device.CreateTexture(depth);
    impl.shadowMatrixBuffer=device.CreateBuffer({sizeof(RendererShadowConstants),0,R::BufferUsage::Constant,R::ResourceState::ConstantBuffer});
    Check(impl.shadowPipeline && impl.shadowDepthTarget && impl.shadowMatrixBuffer,"Shadow fixture resources failed");

    MeshData mesh;mesh.vertices.resize(3);mesh.indices={0,1,2};
    mesh.vertices[0].position={-.25f,-.25f,.25f};mesh.vertices[1].position={.25f,-.25f,.25f};mesh.vertices[2].position={0,.25f,.25f};
    Scene scene;auto first=scene.Add(mesh);auto second=scene.AddInstance(first,Math::Translation({4,0,0}));
    Check(bool(first) && bool(second),"Shadow fixture scene failed");
    impl.m_meshes.resize(scene.Meshes().size());
    impl.m_meshes[0].source=scene.Meshes()[0];impl.m_meshes[0].indexCount=3;
    impl.materialStates.resize(scene.Materials().size());
    R::TextureDesc color;color.width=color.height=1;color.format=R::Format::R8G8B8A8_UNORM;color.usage=R::TextureUsage::ShaderResource;
    auto* texture=resources.Keep(device.CreateTexture(color));Check(texture!=nullptr,"Coverage texture failed");
    for(auto& material:impl.materialStates)material.textures[0]=texture;
    for(uint32_t i=0;i<2;++i) {
        Renderer::Impl::ShadowCaster caster;caster.entity=i;caster.mesh=0;
        caster.material=ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(i)));caster.indexCount=3;
        const float x=4.f*i;caster.worldBounds.bounds={{x-.25f,-.25f,.2f},{x+.25f,.25f,.3f},true};
        caster.worldBounds.magnitude={x+1,1,1};impl.shadowCasterScratch.push_back(caster);
    }
    Renderer::Impl::ShadowData shadows;shadows.viewCount=2;shadows.columns=2;shadows.resolution=64;
    shadows.constants.lightViewProjectionMatrix[0]=Math::float4x4::Identity();
    shadows.constants.lightViewProjectionMatrix[1]=Math::Translation({-4,0,0});
    const auto reuse=[&] {return impl.ReuseShadowAtlas(scene,shadows,nullptr);};
    const auto commit=[&] {impl.CommitShadowAtlas(scene);};
    Check(!reuse() && shadows.dirtyViews[0] && shadows.dirtyViews[1],"Initial atlas must render both views");
    std::vector<uint8_t> expected;
    const auto referenceAppend=[&](const auto& value) {
        const auto* first=reinterpret_cast<const uint8_t*>(&value);
        expected.insert(expected.end(),first,first+sizeof(value));
    };
    for(const auto& caster:impl.shadowCasterScratch) {
        const auto entity=static_cast<EntityID>(caster.entity);
        const auto& source=impl.m_meshes[caster.mesh];
        const auto& mat=scene.Materials()[caster.material];
        const auto& state=impl.materialStates[caster.material];
        referenceAppend(caster.entity);referenceAppend(caster.mesh);referenceAppend(caster.material);
        referenceAppend(reinterpret_cast<uintptr_t>(source.vertexBuffer));
        referenceAppend(reinterpret_cast<uintptr_t>(source.indexBuffer));referenceAppend(source.indexCount);
        referenceAppend(caster.arena);referenceAppend(caster.compact);referenceAppend(caster.coverageFlags);
        referenceAppend(scene.GetTransform(entity).m);referenceAppend(mat.alphaMode);
        referenceAppend(mat.baseColor.w);referenceAppend(mat.alphaCutoff);referenceAppend(mat.doubleSided);
        referenceAppend(reinterpret_cast<uintptr_t>(state.textures[0]));
        referenceAppend(reinterpret_cast<uintptr_t>(Detail::ImageDdsAccess::Identity(mat.baseColorTexture)));
        referenceAppend(mat.baseColorTexture.GetWidth());referenceAppend(mat.baseColorTexture.GetHeight());
        referenceAppend(mat.baseColorTexture.GetColorSpace());
    }
    Check(impl.shadowAtlasCache.pendingCasterBytes==expected,"Caster signature bytes differ from insert reference");
    commit();
    Check(reuse() && !shadows.dirtyViews[0] && !shadows.dirtyViews[1],"Static atlas was not reused");commit();

    shadows.casterVolumes[0].count=1;
    shadows.casterVolumes[0].planes[0]={1,0,0,2};
    Check(reuse(),"Receiver volume motion with identical projected casters must reuse the atlas");commit();
    shadows.casterVolumes[0].planes[0][3]=3;
    Check(reuse(),"Changed culling plane unnecessarily invalidated identical shadow contents");commit();
    shadows.casterVolumes[0].planes[0][3]=-1;
    Check(!reuse() && shadows.dirtyViews[0] && !shadows.dirtyViews[1],
        "Receiver volume removing a caster must invalidate only its tile");commit();
    shadows.casterVolumes[0].count=0;
    Check(!reuse() && shadows.dirtyViews[0] && !shadows.dirtyViews[1],
        "Receiver volume restoring a caster must invalidate only its tile");commit();
    Check(reuse(),"Restored receiver membership failed to become reusable");commit();

    shadows.constants.lightViewProjectionMatrix[1]=Math::Translation({-4,.1f,0});
    Check(!reuse() && !shadows.dirtyViews[0] && shadows.dirtyViews[1],"Light matrix change did not isolate its view");commit();
    Check(reuse(),"Committed light matrix did not become reusable");commit();
    Check(first.SetPosition({0,.1f,0}),"Transform update failed");
    Check(!reuse() && shadows.dirtyViews[0] && !shadows.dirtyViews[1],"Changed caster dirtied an unrelated view or remained cached");commit();
    Check(reuse(),"Committed transform did not become reusable");commit();
    auto material=first.GetMaterial();material.alphaCutoff=.25f;Check(first.SetMaterial(material),"Material update failed");
    Check(!reuse() && shadows.dirtyViews[0] && !shadows.dirtyViews[1],"Coverage change did not invalidate its view");
    Check(!reuse() && shadows.dirtyViews[0],"Uncommitted metadata became a reuse baseline");commit();
    Check(reuse(),"Committed coverage did not become reusable");commit();

    auto replacement=std::make_shared<MeshData>(mesh);impl.m_meshes[0].source=replacement;
    Check(!reuse() && shadows.dirtyViews[0] && shadows.dirtyViews[1],"Mesh owner replacement was not detected");commit();
    replacement.reset();Check(!reuse(),"Expired mesh owner remained reusable");
    impl.m_meshes[0].source=scene.Meshes()[0];Check(!reuse(),"Restored mesh skipped expired-owner invalidation");commit();
    Check(reuse(),"Restored committed mesh was not reusable");commit();

    const auto saved=impl.shadowCasterScratch.front();impl.shadowCasterScratch.erase(impl.shadowCasterScratch.begin());
    Check(!reuse() && shadows.dirtyViews[0] && !shadows.dirtyViews[1],"Caster removal did not update view membership");commit();
    impl.shadowCasterScratch.insert(impl.shadowCasterScratch.begin(),saved);
    Check(!reuse() && shadows.dirtyViews[0] && !shadows.dirtyViews[1],"Caster reappearance was not detected");commit();
    Check(reuse(),"Restored caster membership did not become reusable");commit();
    impl.shadowCasterScratch[0].firstIndex=1;
    Check(!reuse() && shadows.dirtyViews[0] && !shadows.dirtyViews[1],"Geometry range change remained cached");commit();
    Check(reuse(),"Committed geometry range did not become reusable");
    for(uint32_t view=0;view<2;++view) {
        std::printf("View %u signature bytes: %zu\n",view,impl.shadowAtlasCache.views[view].pendingBytes.size());
        Check(impl.shadowAtlasCache.views[view].pendingBytes.size()<=128,
            "Per-view signatures must not duplicate full caster metadata");
    }
    config.lighting.reuseStaticShadows=false;impl.config=config;
    Check(!reuse() && !impl.shadowAtlasCache.pending,"Disabled cache retained a pending commit");
    config.lighting.reuseStaticShadows=true;impl.config=config;
    Check(!reuse() && shadows.dirtyViews[0] && shadows.dirtyViews[1],"Re-enabled cache reused invalid state");commit();
    Check(reuse(),"Re-enabled committed atlas was not reusable");
    std::puts("PASS shadow atlas cache: exact changes, per-view isolation, commit boundary, owner lifetime and compact view signatures");
    return 0;
}
catch(const std::exception& error) {std::fprintf(stderr,"FAIL ShadowAtlasCache: %s\n",error.what());return 1;}
