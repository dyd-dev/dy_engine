#define main RendererExceptionEntryUnused
#include "RendererExceptionTests.cpp"
#undef main
#include "../src/Extends/AdvancedRendering/AdvancedRenderingInternal.h"

namespace {
struct QueueConfig final : dyf::RenderExtension
{
    bool PrepareCamera(dyf::RenderFrame& frame) override
    {
        auto next=frame.renderer.GetConfig();next.advanced.enabled=true;
        return frame.renderer.SetConfig(next);
    }
};
struct ObserveConfig final : dyf::RenderExtension
{
    unsigned checked=0;
    bool Prepare(dyf::RenderFrame& frame,R::ICommandList&) override
    {
        Check(frame.renderer.GetConfig().advanced.enabled,"Callback did not queue requested settings");
        Check(!frame.renderer.GetAppliedConfig().advanced.enabled,"Callback applied settings inside the current frame");
        Check(!dyf::Detail::AdvancedModuleEnabled(frame.renderer),"Advanced module used pending settings inside current frame");
        ++checked;return true;
    }
};
}
int main()
{
    try
    {
        Device device;
        dyf::RendererConfig config;config.enableProfilerHud=false;
        config.enableHdrRendering=true;config.lighting.shadows=false;
        auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);
        Check(bool(renderer),"Renderer creation failed");
        auto& state=*renderer->m_impl;
        const uint8_t code[]={1};
        state.vertexShader=device.CreateShader({R::ShaderStage::Vertex,"main",code,1});
        state.fragmentShader=device.CreateShader({R::ShaderStage::Fragment,"main",code,1});
        state.toneVertexShader=device.CreateShader({R::ShaderStage::Vertex,"main",code,1});
        state.toneFragmentShader=device.CreateShader({R::ShaderStage::Fragment,"main",code,1});
        Check(renderer->AddExtension(std::make_unique<QueueConfig>()),"Queue extension registration failed");
        auto observer=std::make_unique<ObserveConfig>();auto* observed=observer.get();
        Check(renderer->AddExtension(std::move(observer)),"Observer extension registration failed");
        dyf::MeshData mesh;mesh.vertices.resize(3);mesh.indices={0,1,2};
        mesh.vertices[0].position={-.2f,-.2f,-2};mesh.vertices[1].position={.2f,-.2f,-2};mesh.vertices[2].position={0,.2f,-2};
        dyf::Scene scene;Check(bool(scene.Add(mesh)),"Scene setup failed");dyf::Camera camera;
        auto* target=device.CreateTexture({16,16,1,1,R::Format::R8G8B8A8_UNORM,R::TextureUsage::RenderTarget|R::TextureUsage::ShaderResource});
        Check(renderer->RenderToTexture(scene,camera,target,R::ResourceState::Undefined),"Current frame failed after queued settings");
        Check(observed->checked==1 && !renderer->GetAppliedConfig().advanced.enabled && renderer->GetConfig().advanced.enabled,
            "Requested and applied configuration boundary changed during Render");
        device.DestroyTexture(target);
        std::puts("PASS configuration queued in PrepareCamera stays pending for downstream modules.");
        return 0;
    }
    catch(const std::exception& error)
    {std::fprintf(stderr,"Renderer configuration test failed: %s\n",error.what());return 1;}
}
