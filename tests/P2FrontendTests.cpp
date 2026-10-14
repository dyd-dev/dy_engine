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
// Test-only access: production classes do not expose failure injection hooks.
#define private public
#include <dyf/Renderer.h>
#include <dyf/Scene.h>
#undef private
#include <dyf/Camera.h>
#include <dyf/Canvas.h>
#include <dyf/RHI.h>
#include "Backends/Null/NullDevice.h"
#include "dyf/ShaderLayout.h"

namespace {
int failAllocation = -1;
const void* watchedAllocation = nullptr;
bool watchedFreed = false;
void Check(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
}
void* operator new(std::size_t bytes) {
    if(failAllocation == 0) { failAllocation = -1; throw std::bad_alloc(); }
    if(failAllocation > 0) --failAllocation;
    if(void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    if(p && p == watchedAllocation) watchedFreed = true;
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
namespace RHI = dyf::RHI;
struct Query : RHI::TimestampQuery { Query() : TimestampQuery(2) {} };
struct Device : dyf::Backends::NullDevice {
    int failShader = 0, shaderCalls = 0, shadersLive = 0;
    int failTexture = 0, textureCalls = 0, throwBuffer = 0, bufferCalls = 0;
    bool throwSet = false, throwUpload = false, queriesEnabled = false, bindlessEnabled = false;
    int queriesLive = 0, failQuery = 0, queryCalls = 0, presentThrows = 0;
    Device() { Check(Initialize(nullptr,{}) == 0, "Null initialization"); }
    ~Device() { ReleaseResources(); }
    bool CreateSwapchainNative(const RHI::SwapchainDesc& source) override {
        auto desc=source;
        if(!desc.initialWidth) desc.initialWidth=16;
        if(!desc.initialHeight) desc.initialHeight=16;
        return NullDevice::CreateSwapchainNative(desc);
    }
    bool PresentNative() override {
        if(presentThrows>0) { --presentThrows; throw std::runtime_error("expected presentation failure"); }
        return NullDevice::PresentNative();
    }
    RHI::ShaderHandle CreateShaderNative(const RHI::ShaderDesc& desc) override {
        if(++shaderCalls == failShader) return nullptr;
        auto result = NullDevice::CreateShaderNative(desc);
        if(result) ++shadersLive;
        return result;
    }
    void DestroyShaderNative(RHI::ShaderHandle shader) override {
        --shadersLive; NullDevice::DestroyShaderNative(shader);
    }
    RHI::TextureHandle CreateTextureNative(const RHI::TextureDesc& desc) override {
        if(++textureCalls == failTexture) return nullptr;
        return NullDevice::CreateTextureNative(desc);
    }
    RHI::BufferHandle CreateBufferNative(const RHI::BufferDesc& desc) override {
        if(++bufferCalls == throwBuffer) throw std::runtime_error("expected buffer creation failure");
        return NullDevice::CreateBufferNative(desc);
    }
    RHI::ResourceSetHandle CreateResourceSetNative(const RHI::ResourceSetDesc& desc) override {
        if(throwSet) throw std::runtime_error("expected resource set failure");
        return NullDevice::CreateResourceSetNative(desc);
    }
    bool UpdateTextureNative(RHI::ICommandList& cmd,RHI::TextureHandle texture,uint32_t mip,uint32_t layer,
        const void* bytes,uint32_t size,uint32_t row,uint32_t slice) override {
        if(throwUpload) throw std::runtime_error("expected upload recording failure");
        return NullDevice::UpdateTextureNative(cmd,texture,mip,layer,bytes,size,row,slice);
    }
    bool SupportsNative(RHI::Feature feature) const override {
        if(feature == RHI::Feature::DescriptorIndexing) return bindlessEnabled;
        return feature == RHI::Feature::TimestampQuery ? queriesEnabled : NullDevice::SupportsNative(feature);
    }
    RHI::TimestampQueryHandle CreateTimestampQueryNative(const RHI::TimestampQueryDesc&) override {
        if(++queryCalls == failQuery) throw std::runtime_error("expected query creation failure");
        ++queriesLive; return new Query;
    }
    void DestroyTimestampQueryNative(RHI::TimestampQueryHandle query) override {
        --queriesLive; delete static_cast<Query*>(query);
    }
    double GetTimestampPeriodNative() const override { return 1; }
    uint32_t GetTimestampValidBitsNative() const override { return 64; }
};
dyf::RendererShaderDesc Shaders() {
    static const uint8_t bytes[] = {1,2,3,4};
    dyf::RendererShaderDesc desc;
    for(auto* s : {&desc.meshVertex,&desc.shadowVertex,&desc.canvasVertex,&desc.toneMapVertex})
        *s = {RHI::ShaderStage::Vertex,"main",bytes,sizeof(bytes)};
    for(auto* s : {&desc.meshFragment,&desc.canvasFragment,&desc.toneMapFragment})
        *s = {RHI::ShaderStage::Fragment,"main",bytes,sizeof(bytes)};
    return desc;
}
std::unique_ptr<dyf::Renderer> Renderer(Device& device,bool shadows=false) {
    dyf::RendererConfig config;
    config.enableHdrRendering = false;
    config.lighting.shadows = shadows;
    config.enableProfilerHud = false;
    auto renderer = std::unique_ptr<dyf::Renderer>(new dyf::Renderer(device,nullptr,config));
    Check(renderer->SetShaders(Shaders()) && renderer->ApplySettings(),"Shader setup");
    return renderer;
}
void Swapchain(Device& device) {
    RHI::SwapchainDesc desc; desc.initialWidth=16; desc.initialHeight=16;
    desc.format=RHI::Format::B8G8R8A8_UNORM;
    Check(device.CreateSwapchain(desc),"Swapchain setup");
}
void ShaderViews() {
    Device device; auto renderer=Renderer(device);
    auto next=Shaders(); const uint8_t bytes[]={9,8,7,6};
    next.meshFragment.binary=bytes;
    next.meshFragment.entryPoint="pendingEntry";
    Check(renderer->SetShaders(next),"First pending override");
    auto saved=renderer->GetShaders();
    watchedAllocation=saved.meshFragment.binary; watchedFreed=false;
    Check(renderer->SetShaders(Shaders()),"Cancel pending override");
    watchedAllocation=nullptr;
    // Never dereference a released pointer, even in the regression's red run.
    Check(!watchedFreed,"GetShaders storage released before settings application");
    Check(std::memcmp(saved.meshFragment.binary,bytes,sizeof(bytes))==0,"Saved shader contents changed");
    Check(std::strcmp(saved.meshFragment.entryPoint,"pendingEntry")==0,"Saved entry point changed");
    Check(renderer->SetShaders(saved),"Restore saved view");
}
void CanvasRetry() {
    Device device;
    {
        auto renderer=Renderer(device);
        device.failShader=2;
        Check(!renderer->InitializeCanvas(RHI::Format::B8G8R8A8_UNORM),"Canvas partial shader failure");
        device.failShader=0;
        Check(renderer->InitializeCanvas(RHI::Format::B8G8R8A8_UNORM),"Canvas retry failed");
    }
    Check(device.shadersLive==0,"Canvas retry lost a shader owner");
}
void CanvasException() {
    Device device; Swapchain(device); auto renderer=Renderer(device);
    Check(renderer->InitializeCanvas(RHI::Format::B8G8R8A8_UNORM),"Canvas initialization");
    const auto before=device.GetResourceAllocationCounters();
    device.throwSet=true;
    Check(!renderer->Render(dyf::Canvas(16,16)),"Canvas exception must return false");
    Check(device.m_userCommands.empty(),"Canvas command owner leaked");
    Check(device.GetResourceAllocationCounters().textures.live==before.textures.live,"Canvas texture owner leaked");
}
void CanvasRecovery() {
    for(bool throwDuringRecovery : {false,true}) {
        Device device; Swapchain(device); auto renderer=Renderer(device);
        Check(!renderer->Render(dyf::Canvas(0,0)),"Invalid canvas must fail");
        Check(!device.m_frameActive,"Invalid canvas acquired a frame");
        device.throwSet=true;
        device.presentThrows=throwDuringRecovery?1:0;
        Check(!renderer->Render(dyf::Canvas(16,16)),"Canvas recording must fail");
        Check(device.m_frameActive==throwDuringRecovery,"Canvas failure left unexpected active frame");
        device.throwSet=false;
        Check(renderer->SetVSync(!renderer->GetConfig().vsync),"Queue output settings");
        Check(renderer->Render(dyf::Canvas(16,16)),"Canvas failure blocked output settings/retry");
        Check(!device.m_frameActive,"Successful retry did not end frame");
    }
}
void DefaultTextureRetry() {
    Device device; Swapchain(device); auto renderer=Renderer(device);
    device.failTexture=2;
    Check(!renderer->InitializeMesh(),"Fallback texture failure not observed");
    device.failTexture=0;
    Check(renderer->InitializeMesh(),"Mesh retry failed");
    for(auto* texture : renderer->defaultMaterialTextures) Check(texture!=nullptr,"Fallback texture still missing");
    for(auto* texture : renderer->defaultMaterialTextures)
        Check(device.m_resourceStates.at({reinterpret_cast<uintptr_t>(texture),0,0})==RHI::ResourceState::ShaderResource,
            "Fallback upload not committed before cache hit");
}
void FrameUploadRollback() {
    Device device; Swapchain(device); auto renderer=Renderer(device,true);
    Check(renderer->InitializeMesh(),"Mesh initialization");
    auto* oldLight=device.CreateBuffer({64,0,RHI::BufferUsage::Constant,RHI::ResourceState::ConstantBuffer});
    auto* oldShadow=device.CreateBuffer({64,0,RHI::BufferUsage::Constant,RHI::ResourceState::ConstantBuffer});
    renderer->lightingBuffer=oldLight; renderer->shadowMatrixBuffer=oldShadow;
    const auto before=device.GetResourceAllocationCounters();
    device.throwBuffer=device.bufferCalls+2;
    dyf::Scene scene; dyf::Camera camera;
    Check(!renderer->Render(scene,camera),"Frame upload exception must return false");
    Check(renderer->lightingBuffer==oldLight && renderer->shadowMatrixBuffer==oldShadow,"Prior frame buffers not restored");
    Check(device.m_userCommands.empty(),"Frame upload command owner leaked");
    Check(device.GetResourceAllocationCounters().buffers.live==before.buffers.live,"New frame buffer owner leaked");
}
void ProfilerCancellation() {
    Device device; Swapchain(device); auto renderer=Renderer(device);
    Check(renderer->InitializeMesh(),"Mesh initialization");
    device.queriesEnabled=true; device.failQuery=2;
    dyf::Scene scene; dyf::Camera camera;
    Check(!renderer->Render(scene,camera),"Second sample creation must fail");
    Check(renderer->m_pending.empty() && device.queriesLive==0,"Unsubmitted sample blocks profiler FIFO");
    device.failQuery=0;
    // Null's native command list rejects timestamp replay, exercising the bool
    // submission failure path after both real common-RHI query owners exist.
    Check(!renderer->Render(scene,camera),"Unsupported native timestamp replay must fail");
    Check(renderer->m_pending.empty() && device.queriesLive==0,"Failed submission retained profiler samples");
    device.queriesEnabled=false;
    Check(renderer->Render(scene,camera),"Rendering did not recover after sample cancellation");
}
struct Scene : dyf::Scene { using dyf::Scene::CreateEntity; };
void SceneAppend() {
    for(int allocation=0;allocation<4;++allocation) {
        Scene scene;
        failAllocation=allocation;
        try { scene.CreateEntity(dyf::MeshID::Invalid,dyf::MaterialID::Invalid); }
        catch(const std::bad_alloc&) {}
        failAllocation=-1;
        Check(scene.GetEntityCount()==0,"Failed append published a partial entity");
        Check(scene.m_entityMaterials.empty() && scene.m_entityTransforms.empty() && scene.m_entityLighting.empty(),
            "Failed append left unequal parallel arrays");
        Check(dyf::ToIndex(scene.CreateEntity(dyf::MeshID::Invalid,dyf::MaterialID::Invalid))==0,"Retry entity ID changed");
    }
}
void TextureException() {
    Device device; auto renderer=Renderer(device); dyf::Scene scene;
    dyf::MeshData mesh; mesh.vertices.resize(3); mesh.indices={0,1,2};
    dyf::MaterialDesc material; material.baseColorTexture=dyf::Image(1,1,{255,255,255,255});
    Check(bool(scene.Add(mesh,material)),"Scene setup");
    device.throwUpload=true;
    try { (void)renderer->SyncTextures(scene,&device); } catch(const std::exception&) {}
    Check(device.m_userCommands.empty(),"Texture upload command owner leaked");
    Check(renderer->m_textures[0].state==RHI::ResourceState::Undefined,"Failed upload published success");
    device.throwUpload=false;
    Check(renderer->SyncTextures(scene,&device),"Texture upload retry failed");
}
void DrawDataException() {
    Device device; Swapchain(device); auto renderer=Renderer(device);
    device.bindlessEnabled=true;
    renderer->fragmentShader=device.CreateShader(Shaders().meshFragment);
    renderer->shaderSources.bytes[dyf::Renderer::MeshFragment].clear();
    Check(renderer->InitializeMesh(),"Mesh initialization");
    renderer->lightingBuffer=device.CreateBuffer({sizeof(dyf::RendererLightingConstants),0,RHI::BufferUsage::Constant,RHI::ResourceState::ConstantBuffer});
    // Force bindless material-page upload, then fail the first resource-set creation.
    renderer->materialStates.resize(1);
    renderer->materialStates[0].textures.fill(renderer->defaultMaterialTextures[0]);
    Check(renderer->UsesBindlessMaterials(),"Bindless test setup rejected");
    dyf::Scene scene; auto* commands=device.AcquireCommandList();
    const auto before=device.GetResourceAllocationCounters();
    device.throwSet=true;
    bool injected=false;
    try { (void)renderer->RecordMainPass(scene,dyf::Camera{},*commands,nullptr,nullptr,device.GetBackBuffer()); }
    catch(const std::exception&) { injected=true; }
    device.DestroyCommandList(commands);
    Check(injected,"Material set failure was not reached");
    Check(device.GetResourceAllocationCounters().buffers.live==before.buffers.live,"Material index buffer owner leaked");
}
void ShadowAngles() {
    Device device; auto renderer=Renderer(device,true); dyf::Scene scene; dyf::Camera camera;
    Check(camera.LookAt({0,0,0},{0,0,-1},{0,1,0}) && camera.SetPerspective(1,3.10f,.1f,10),"Wide camera setup");
    dyf::DirectionalLight light; light.direction={0,0,1}; Check(bool(scene.Add(light)),"Light setup");
    dyf::Renderer::ShadowData shadows; renderer->BuildShadows(shadows,scene,camera);
    const float far=shadows.constants.directionalSplits[0][0];
    const float extent=far/camera.projection.m[5];
    const auto projected=dyf::Math::TransformPoint(shadows.constants.lightViewProjectionMatrix[0],{extent,extent,-far});
    Check(std::abs(projected.x)<=1.001f && std::abs(projected.y)<=1.001f,"Wide camera receiver outside cascade");
}
void SpotAngles() {
    Device device; auto renderer=Renderer(device,true); dyf::Camera camera;
    dyf::Renderer::ShadowData shadows;
    dyf::Scene spotScene; dyf::SpotLight spot; spot.position={0,0,0}; spot.direction={0,0,-1}; spot.castShadow=true;
    auto handle=spotScene.Add(spot); Check(bool(handle),"Spot setup");
    struct Case {float inner,outer,effective;};
    const Case cases[]={{.2f,.5f,.5f},{1.55f,.3f,1.55f},{3.f,2.f,1.55334306f},
        {std::numeric_limits<float>::quiet_NaN(),.5f,.5f}};
    for(const auto& value:cases) {
        spot.innerConeRadians=value.inner; spot.outerConeRadians=value.outer;
        Check(handle.Set(spot),"Spot update"); renderer->BuildShadows(shadows,spotScene,camera);
        Check(shadows.viewCount==1,"Spot shadow view missing");
        const auto edge=dyf::Math::TransformPoint(shadows.constants.lightViewProjectionMatrix[0],{std::tan(value.effective),0,-1});
        Check(std::abs(edge.x-1)<.001f,"Spot shadow does not cover normalized lighting cone");
    }
}
}
int main(int argc,char** argv) {
    try {
        Check(argc==2,"Specify one bounded scenario");
        const std::string name=argv[1];
        if(name=="shader-views") ShaderViews();
        else if(name=="canvas-retry") CanvasRetry();
        else if(name=="canvas-exception") CanvasException();
        else if(name=="canvas-recovery") CanvasRecovery();
        else if(name=="default-texture-retry") DefaultTextureRetry();
        else if(name=="frame-upload-rollback") FrameUploadRollback();
        else if(name=="profiler-cancel") ProfilerCancellation();
        else if(name=="scene-append") SceneAppend();
        else if(name=="texture-exception") TextureException();
        else if(name=="draw-data-exception") DrawDataException();
        else if(name=="shadow-angles") ShadowAngles();
        else if(name=="spot-angles") SpotAngles();
        else throw std::runtime_error("Unknown scenario");
        std::printf("PASS %s\n",name.c_str()); return 0;
    } catch(const std::exception& error) {
        failAllocation=-1; watchedAllocation=nullptr;
        std::fprintf(stderr,"FAIL %s\n",error.what()); return 1;
    }
}
