#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
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
// Test-only access for constructing bounded skin inputs and observing command ownership.
// Production APIs carry no failure-injection hooks.
#define private public
#include <dyf/Extends/Model/ModelRenderer.h>
#include <dyf/Extends/Model/ModelScene.h>
#undef private
#include <dyf/RHI.h>
#include "Backends/Null/NullDevice.h"

namespace { bool failNextAllocation = false; }
void* operator new(std::size_t bytes) {
    if(failNextAllocation) { failNextAllocation = false; throw std::bad_alloc(); }
    if(void* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {
namespace RHI = dyf::RHI;
void Check(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
struct ComputePipeline : RHI::Pipeline {
    explicit ComputePipeline(const RHI::PipelineLayoutDesc& layout) : Pipeline(layout, true) {}
};
struct Query : RHI::TimestampQuery { Query() : TimestampQuery(2) {} };
// Native GPU work is inert; real common RHI recording, state checks, ownership and submission run.
struct Commands : RHI::ICommandList {
    bool hasDraw = false;
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
    void DrawInstancedNative(uint32_t, uint32_t, uint32_t, uint32_t) override { hasDraw = true; }
    void DrawIndexedInstancedNative(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override { hasDraw = true; }
    void ResetTimestampsNative(RHI::TimestampQueryHandle, uint32_t, uint32_t) override {}
    void WriteTimestampNative(RHI::TimestampQueryHandle, uint32_t) override {}
    bool CloseNative() override { return true; }
};
struct Device : dyf::Backends::NullDevice {
    std::string failure;
    unsigned bufferCalls = 0, setsLive = 0, shadersLive = 0, queriesLive = 0;
    uint64_t completed = 0, lastSubmission = 0;
    bool armAfterSubmit = false, delayCompletion = false, completeOnPoll = false, drawFrames = false; std::map<RHI::BufferHandle, std::vector<uint8_t>> uploaded; unsigned setCalls = 0, updateCalls = 0;
    std::vector<RHI::ICommandList*> pending;
    Device() { Check(Initialize(nullptr, {}) == 0, "Null initialization failed"); }
    ~Device() { Complete(); ReleaseResources(); }
    void Complete() {
        completed = lastSubmission;
        for(auto* commands : pending) DiscardCommandListNative(commands);
        pending.clear();
    }
    bool SupportsNative(RHI::Feature feature) const override {
        return feature == RHI::Feature::Compute || feature == RHI::Feature::TimestampQuery || NullDevice::SupportsNative(feature);
    }
    bool CreateSwapchainNative(const RHI::SwapchainDesc& desc) override {
        auto bounded = desc; bounded.initialWidth = bounded.initialHeight = 4;
        return NullDevice::CreateSwapchainNative(bounded);
    }
    bool PresentNative() override { return failure != "present-false" && NullDevice::PresentNative(); }
    bool BeginFrameNative() override {
        // If this deque push did not allocate, disarm before shader restoration can allocate.
        failNextAllocation = false;
        return drawFrames ? NullDevice::BeginFrameNative() : false; // Explicit skipped-frame coverage.
    }
    RHI::ICommandList* AcquireCommandListNative() override { return new Commands; }
    void DiscardCommandListNative(RHI::ICommandList* commands) override { delete static_cast<Commands*>(commands); }
    bool SubmitNative(RHI::ICommandList** commands, uint32_t count) override {
        if(failure == "submit-false") return false;
        if(failure == "main-submit-false")
            for(uint32_t index = 0; index < count; ++index)
                if(static_cast<Commands*>(commands[index])->hasDraw) return false;
        ++lastSubmission;
        for(uint32_t index = 0; index < count; ++index) {
            if(delayCompletion) pending.push_back(commands[index]);
            else DiscardCommandListNative(commands[index]);
        }
        if(!delayCompletion) completed = lastSubmission;
        // No common-RHI allocation follows native Submit success; the next allocation in
        // Prepare is the sample deque's ownership handoff, independent of its block size.
        if(armAfterSubmit) failNextAllocation = true;
        return true;
    }
    uint64_t GetCompletedSubmissionNative() override { if(completeOnPoll) Complete(); return completed; }
    uint64_t GetLastSubmissionNative() const override { return lastSubmission; }
    RHI::ShaderHandle CreateShaderNative(const RHI::ShaderDesc& desc) override {
        auto* shader = NullDevice::CreateShaderNative(desc); if(shader) ++shadersLive; return shader;
    }
    void DestroyShaderNative(RHI::ShaderHandle shader) override { --shadersLive; NullDevice::DestroyShaderNative(shader); }
    bool UpdateTextureNative(RHI::ICommandList&, RHI::TextureHandle, uint32_t, uint32_t,
        const void*, uint32_t, uint32_t, uint32_t) override { return true; }
    RHI::PipelineHandle CreateComputePipelineNative(const RHI::ComputePipelineDesc& desc) override {
        if(failure == "pipeline-throw") throw std::bad_alloc();
        return new ComputePipeline(desc.layout);
    }
    void DestroyPipelineNative(RHI::PipelineHandle pipeline) override {
        if(pipeline->IsCompute()) delete static_cast<ComputePipeline*>(pipeline);
        else NullDevice::DestroyPipelineNative(pipeline);
    }
    RHI::BufferHandle CreateBufferNative(const RHI::BufferDesc& desc) override {
        ++bufferCalls;
        if(bufferCalls == 2 && failure == "buffer-throw") throw std::bad_alloc();
        if(bufferCalls == 2 && failure == "buffer-null") return nullptr;
        return NullDevice::CreateBufferNative(desc);
    }
    bool UpdateBufferNative(RHI::ICommandList&, RHI::BufferHandle buffer, uint32_t, const void* data, uint32_t size) override { ++updateCalls; const auto* bytes = static_cast<const uint8_t*>(data); uploaded[buffer] = {bytes, bytes + size}; return failure != "update-false"; }
    RHI::ResourceSetHandle CreateResourceSetNative(const RHI::ResourceSetDesc& desc) override {
        ++setCalls; if(failure == "set-throw") throw std::bad_alloc();
        if(failure == "set-null") return nullptr;
        auto* set = NullDevice::CreateResourceSetNative(desc); if(set) ++setsLive; return set;
    }
    void DestroyResourceSetNative(RHI::ResourceSetHandle set) override { --setsLive; NullDevice::DestroyResourceSetNative(set); }
    RHI::TimestampQueryHandle CreateTimestampQueryNative(const RHI::TimestampQueryDesc&) override {
        if(failure == "query-throw") throw std::bad_alloc();
        ++queriesLive; return new Query;
    }
    void DestroyTimestampQueryNative(RHI::TimestampQueryHandle query) override { --queriesLive; delete static_cast<Query*>(query); }
    bool ReadTimestampsNative(RHI::TimestampQueryHandle, uint32_t, uint32_t, uint64_t*) override { return false; }
};
void MakeScene(dyf::ModelScene& scene) {

    dyf::MeshData triangle; triangle.vertices.resize(3); triangle.indices = {0,1,2};
    triangle.vertices[1].position.x = 1.f; triangle.vertices[2].position.y = 1.f;
    Check(bool(scene.Add(triangle)), "scene creation failed");
    scene.m_meshSkinInfluences.resize(1); scene.m_meshSkinInfluences[0].resize(3);
    for(auto& influence : scene.m_meshSkinInfluences[0]) influence.weights.x = 1.f;
    scene.m_entitySkinPaletteOffsets = {0}; scene.m_jointPaletteMatrices.resize(1);

}
void ChangePalette(dyf::ModelScene& scene, float value) { scene.m_jointPaletteMatrices[0].positionMatrix.m[0] = value; }
void Configure(dyf::ModelRenderer& model) {
    static const uint8_t bytes[] = {1}; dyf::ModelShaderDesc shaders;
    shaders.vertex = {RHI::ShaderStage::Vertex, "main", bytes, sizeof(bytes)};
    shaders.shadowVertex = shaders.vertex;
    shaders.skinningCompute = {RHI::ShaderStage::Compute, "main", bytes, sizeof(bytes)};
    Check(model.SetShaders(shaders), "test shaders rejected");
}
void Warm(bool compute) {
    Device device;
    auto renderer = dyf::Renderer::Create(device, reinterpret_cast<void*>(1)); Check(bool(renderer), "renderer creation failed");
    dyf::ModelScene scene; MakeScene(scene);
    const auto baseline = device.GetResourceAllocationCounters().buffers.live;
    {
        dyf::ModelRenderer model(*renderer); Configure(model);
        Check(model.SetSkinningExecutionMode(compute ? dyf::SkinningExecutionMode::ComputePreSkin : dyf::SkinningExecutionMode::VertexShader), "mode rejected");
        Check(model.Render(scene), "first Prepare failed");
        const auto created = device.GetResourceAllocationCounters().buffers.created;
        const auto sets = device.setCalls;
        const auto submissions = device.lastSubmission;
        const auto uploads = device.updateCalls;
        for(unsigned frame = 0; frame < 8; ++frame) Check(model.Render(scene), "warm Prepare failed");
        Check(device.GetResourceAllocationCounters().buffers.created == created, "warm model creates equivalent buffers");
        Check(device.setCalls == sets, "warm model creates equivalent compute sets");
        std::cout << "COUNTERS " << (compute ? "Compute" : "VS")
            << " warm buffer delta=0 compute-set delta=0 slots=" << model.m_frames.size() << '\n';
        if(!compute)
        {
            Check(device.lastSubmission == submissions, "static VS preparation submits empty marker work");
            Check(device.updateCalls == uploads, "unchanged influence/palette bytes are reuploaded");
        }
        ChangePalette(scene, 7.f);
        const auto updates = device.updateCalls;
        Check(model.Render(scene), "palette change rejected");
        Check(device.updateCalls > updates, "changed palette was not uploaded");
        Check(device.GetResourceAllocationCounters().buffers.created == created, "palette change recreates buffers");
        scene.m_meshSkinInfluences[0][0].weights.x = -1.f;
        Check(!model.Render(scene), "cached influences bypass input validation");
        scene.m_meshSkinInfluences[0][0].weights.x = .5f;
        Check(model.Render(scene), "valid influence update rejected");
        const auto influence = model.m_draws[0].vertexResources[0].buffer;
        const auto& bytes = device.uploaded.at(influence);
        dyf::SkinInfluence actual; std::memcpy(&actual, bytes.data(), sizeof(actual));
        Check(actual.weights.x == .5f, "changed influences used stale cache");
        scene.m_entitySkinPaletteOffsets[0] = 1;
        Check(!model.Render(scene), "cached influence validation ignores changed palette offset");
        scene.m_entitySkinPaletteOffsets[0] = 0;
        scene.m_meshSkinInfluences[0][0].jointIndices[0] = UINT32_MAX;
        Check(!model.Render(scene), "largest joint index bypasses cached palette validation");
        scene.m_meshSkinInfluences[0][0].jointIndices[0] = 0;
        dyf::ModelScene other; MakeScene(other);
        other.m_jointPaletteMatrices[0].positionMatrix.m[0] = 13.f;
        Check(model.Render(other), "same-size scene switch rejected");
        const auto otherPalette = model.m_draws[0].vertexResources[1].buffer;
        float matrixValue = 0; std::memcpy(&matrixValue, device.uploaded.at(otherPalette).data(), sizeof(matrixValue));
        Check(matrixValue == 13.f, "same-size scene switch uses stale palette");
    }
    Check(device.GetResourceAllocationCounters().buffers.live == baseline, "destruction retained cached buffers");
    Check(device.setsLive == 0 && device.queriesLive == 0, "destruction retained set/query");
}
void Morph() {
    Device device; auto renderer = dyf::Renderer::Create(device, reinterpret_cast<void*>(1));
    dyf::ModelScene scene; MakeScene(scene); scene.m_entitySkinPaletteOffsets[0] = UINT32_MAX;
    scene.m_entityMorphedMeshes = {*scene.Meshes()[0]};
    dyf::ModelRenderer model(*renderer); Configure(model); Check(model.Render(scene), "morph prepare rejected");
    const auto first = model.m_draws[0].vertexBuffer;
    const auto created = device.GetResourceAllocationCounters().buffers.created;
    scene.m_entityMorphedMeshes[0].vertices[0].position.x = 9.f;
    Check(model.Render(scene), "morph update rejected");
    Check(model.m_draws[0].vertexBuffer == first, "same-size morph recreates vertex buffer");
    float x = 0; std::memcpy(&x, device.uploaded.at(first).data(), sizeof(x));
    Check(x == 9.f, "morph update used stale bytes");
    Check(device.GetResourceAllocationCounters().buffers.created == created, "morph change recreates buffers");
}
void Ring() {
    Device device; device.delayCompletion = true;
    auto renderer = dyf::Renderer::Create(device, reinterpret_cast<void*>(1));
    dyf::ModelScene scene; MakeScene(scene); dyf::ModelRenderer model(*renderer); Configure(model);
    Check(model.Render(scene), "ring first prepare failed");
    const auto firstPalette = model.m_draws[0].vertexResources[1].buffer;
    ChangePalette(scene, 4.f);
    Check(model.Render(scene), "ring second prepare failed");
    Check(model.m_draws[0].vertexResources[1].buffer != firstPalette, "pending palette was overwritten");
    const auto created = device.GetResourceAllocationCounters().buffers.created;
    device.completeOnPoll = true;
    for(unsigned frame = 0; frame < 6; ++frame) Check(model.Render(scene), "completed ring reuse failed");
    Check(device.GetResourceAllocationCounters().buffers.created == created, "completed frame slots were not reused");
    Check(model.m_frames.size() <= device.GetDesc().maxFramesInFlight, "model ring exceeds device frame bound");
    dyf::ModelScene smaller;
    Check(model.Render(smaller), "smaller scene switch rejected");
    for(size_t index = 0; index < model.m_frames.size(); ++index)
        if(index != model.m_frameIndex)
            Check(model.m_frames[index].buffers.empty(), "completed inactive slot retains historical scene buffers");
}
void SkippedCompletion() {
    Device device; device.drawFrames = true;
    dyf::RendererConfig config; config.enableProfilerHud = false;
    auto renderer = dyf::Renderer::Create(device, reinterpret_cast<void*>(1), config);
    dyf::ModelScene scene; MakeScene(scene);
    static const uint8_t code[] = {1};
    dyf::RendererShaderDesc shaders; shaders.meshVertex = {RHI::ShaderStage::Vertex, "main", code, 1};
    shaders.meshFragment = {RHI::ShaderStage::Fragment, "main", code, 1};
    Check(renderer->SetShaders(shaders), "stock test shaders failed");
    Check(renderer->Render(scene), "initial downstream render failed");
    const auto previous = renderer->GetLastRenderCompletion();
    Check(bool(previous), "downstream draw did not publish completion");
    device.drawFrames = false; device.delayCompletion = true;
    dyf::ModelRenderer model(*renderer); Configure(model);
    Check(model.SetSkinningExecutionMode(dyf::SkinningExecutionMode::ComputePreSkin), "compute mode failed");
    Check(model.Render(scene), "skipped downstream render failed");
    Check(renderer->GetLastRenderCompletion() == previous, "skipped draw changed downstream completion");
    Check(model.m_frames[model.m_frameIndex].completion != previous, "stale downstream completion replaced pending Prepare completion");
    Check(!device.IsComplete(model.m_frames[model.m_frameIndex].completion), "pending Prepare completion was lost");
    device.Complete();
}
void DownstreamCompletion(bool failPresent, bool compute = false) {
    Device device; device.drawFrames = true; device.delayCompletion = true;
    dyf::RendererConfig config; config.enableProfilerHud = false;
    auto renderer = dyf::Renderer::Create(device, reinterpret_cast<void*>(1), config);
    static const uint8_t code[] = {1};
    dyf::RendererShaderDesc shaders; shaders.meshVertex = {RHI::ShaderStage::Vertex, "main", code, 1};
    shaders.meshFragment = {RHI::ShaderStage::Fragment, "main", code, 1};
    Check(renderer->SetShaders(shaders), "stock test shaders failed");
    dyf::ModelScene scene; MakeScene(scene);
    dyf::ModelRenderer model(*renderer); Configure(model);
    if(compute) Check(model.SetSkinningExecutionMode(dyf::SkinningExecutionMode::ComputePreSkin), "compute mode rejected");
    if(failPresent) device.failure = "present-false";
    Check(model.Render(scene) == !failPresent, "main render/present failure result incorrect");
    Check(bool(renderer->GetLastRenderCompletion()), "main submission completion missing");
    Check(model.m_frames[model.m_frameIndex].completion == renderer->GetLastRenderCompletion(), "frame lifetime does not cover downstream draw");
    const auto firstPalette = model.m_draws[0].vertexResources[1].buffer;
    device.completed = 1; // Prepare completed, main draw still pending.
    Check(!device.IsComplete(model.m_frames[model.m_frameIndex].completion), "main draw ownership lost after Prepare completion");
    device.failure.clear(); ChangePalette(scene, 3.f);
    Check(model.Render(scene), "second queued main render failed");
    Check(model.m_draws[0].vertexResources[1].buffer != firstPalette, "palette reused while downstream main draw is pending");
    device.Complete(); device.delayCompletion = false;
    Check(model.Render(scene), "completed downstream slot reuse failed");
    const auto created = device.GetResourceAllocationCounters().buffers.created;
    const auto pipelines = device.GetResourceAllocationCounters().pipelines.created;
    const auto sets = device.setCalls;
    for(unsigned frame = 0; frame < 6; ++frame) Check(model.Render(scene), "warm main render failed");
    Check(device.GetResourceAllocationCounters().buffers.created == created, "warm main Model render recreates buffers");
    Check(device.GetResourceAllocationCounters().pipelines.created == pipelines, "warm main Model render rebuilds pipelines");
    Check(device.setCalls == sets, "warm main Model render recreates resource sets");
    const auto restored = renderer->GetShaders();
    Check(renderer->shaderViews.size() <= 2, "warm Model render retains duplicate shader snapshots");
    Check(restored.vertexBindings.empty() && restored.additionalConstantBytes == 0,
        "Model render leaked deformation bindings into stock renderer");
    Check(restored.meshFragment.binarySize == 1 &&
        *static_cast<const uint8_t*>(restored.meshFragment.binary) == 1,
        "Model render replaced caller fragment shader");
    std::cout << "COUNTERS downstream " << (compute ? "Compute" : "VS")
        << " warm buffer delta=0 pipeline delta=0 set delta=0\n";
}
void RotatingSlots(bool compute) {
    Device device; device.drawFrames = true; device.delayCompletion = true;
    dyf::RendererConfig config; config.enableProfilerHud = false;
    auto renderer = dyf::Renderer::Create(device, reinterpret_cast<void*>(1), config);
    static const uint8_t code[] = {1};
    dyf::RendererShaderDesc shaders; shaders.meshVertex = {RHI::ShaderStage::Vertex, "main", code, 1};
    shaders.meshFragment = {RHI::ShaderStage::Fragment, "main", code, 1};
    Check(renderer->SetShaders(shaders), "stock test shaders failed");
    dyf::ModelScene scene; MakeScene(scene);
    dyf::ModelRenderer model(*renderer); Configure(model);
    if(compute) Check(model.SetSkinningExecutionMode(dyf::SkinningExecutionMode::ComputePreSkin), "compute mode rejected");
    Check(model.Render(scene), "rotating first render failed");
    ChangePalette(scene, 2.f); Check(model.Render(scene), "rotating second render failed");
    Check(model.m_frames.size() == 2, "rotating fixture did not populate both frame slots");
    const auto counters = device.GetResourceAllocationCounters();
    const auto sets = device.setCalls;
    for(unsigned frame = 0; frame < 8; ++frame) {
        const auto previousSlot = model.m_frameIndex;
        const auto nextSlot = (previousSlot + 1) % 2;
        device.completed = model.m_frames[nextSlot].completion.m_value;
        Check(!device.IsComplete(model.m_frames[previousSlot].completion), "rotating fixture completed newest main draw");
        ChangePalette(scene, static_cast<float>(frame + 3));
        Check(model.Render(scene), "rotating warm render failed");
        Check(model.m_frameIndex == nextSlot, "model failed to rotate completed frame slots");
    }
    Check(device.GetResourceAllocationCounters().buffers.created == counters.buffers.created,
        "rotating Model slots recreate buffers");
    Check(device.GetResourceAllocationCounters().pipelines.created == counters.pipelines.created,
        "rotating Model slots rebuild pipelines");
    Check(device.setCalls == sets, "rotating Model slots recreate descriptors");
    device.Complete();
    std::cout << "COUNTERS rotating " << (compute ? "Compute" : "VS")
        << " slots=2 warm buffer delta=0 pipeline delta=0 set delta=0\n";
}
void MainSubmissionFailure() {
    Device device; device.drawFrames = true;
    dyf::RendererConfig config; config.enableProfilerHud = false;
    auto renderer = dyf::Renderer::Create(device, reinterpret_cast<void*>(1), config);
    static const uint8_t code[] = {1};
    dyf::RendererShaderDesc shaders; shaders.meshVertex = {RHI::ShaderStage::Vertex, "main", code, 1};
    shaders.meshFragment = {RHI::ShaderStage::Fragment, "main", code, 1};
    Check(renderer->SetShaders(shaders), "stock test shaders failed");
    dyf::ModelScene scene; MakeScene(scene);
    Check(renderer->Render(scene), "initial main render failed");
    const auto previous = renderer->GetLastRenderCompletion();
    device.failure = "main-submit-false"; device.delayCompletion = true;
    dyf::ModelRenderer model(*renderer); Configure(model);
    Check(!model.Render(scene), "main submission failure ignored");
    Check(renderer->GetLastRenderCompletion() == previous, "failed main submission published completion");
    Check(model.m_frames[model.m_frameIndex].completion != previous, "failed main submission lost pending Prepare ownership");
    Check(!device.IsComplete(model.m_frames[model.m_frameIndex].completion), "failed main submission falsely completes Prepare ownership");
    device.Complete(); device.delayCompletion = false; device.failure.clear();
    Check(model.Render(scene), "main submission retry rejected cached prepared resources");
    Check(renderer->GetLastRenderCompletion() != previous, "main retry skipped successful submission");
}
void FailedReuse() {
    Device device; auto renderer = dyf::Renderer::Create(device, reinterpret_cast<void*>(1));
    dyf::ModelScene scene; MakeScene(scene); dyf::ModelRenderer model(*renderer); Configure(model);
    Check(model.Render(scene), "initial prepare failed");
    ChangePalette(scene, 3.f);
    device.failure = "submit-false"; Check(!model.Render(scene), "submission failure ignored");
    device.failure.clear(); Check(model.Render(scene), "failed reusable state was published");
    ChangePalette(scene, 5.f);
    device.failure = "update-false"; Check(!model.Render(scene), "upload failure ignored");
    device.failure.clear(); Check(model.Render(scene), "failed upload was reused");
}
}
int main(int argc, char** argv) {
    try {
        const std::string selected = argc > 1 ? argv[1] : "all";
        for(const auto& test : std::vector<std::pair<std::string, std::function<void()>>>{
            {"vs-warm", []{ Warm(false); }}, {"compute-warm", []{ Warm(true); }},
            {"downstream", []{DownstreamCompletion(false);}}, {"compute-downstream", []{DownstreamCompletion(false, true);}},
            {"vs-rotating", []{RotatingSlots(false);}}, {"compute-rotating", []{RotatingSlots(true);}},
            {"main-submit-failure", MainSubmissionFailure},
            {"present-failure", []{DownstreamCompletion(true);}}, {"skipped-completion", SkippedCompletion}, {"morph", Morph}, {"ring", Ring}, {"failed-reuse", FailedReuse}}) {
            if(selected != "all" && selected != test.first) continue;
            test.second(); std::cout << "PASS " << test.first << '\n';
        }
        return 0;
    } catch(const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
