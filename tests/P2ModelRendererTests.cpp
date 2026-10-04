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
    void DrawIndexedInstancedNative(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
    void ResetTimestampsNative(RHI::TimestampQueryHandle, uint32_t, uint32_t) override {}
    void WriteTimestampNative(RHI::TimestampQueryHandle, uint32_t) override {}
    bool CloseNative() override { return true; }
};
struct Device : dyf::Backends::NullDevice {
    std::string failure;
    unsigned bufferCalls = 0, setsLive = 0, shadersLive = 0, queriesLive = 0;
    uint64_t completed = 0, lastSubmission = 0;
    bool armAfterSubmit = false, delayCompletion = false;
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
    bool BeginFrameNative() override {
        // If this deque push did not allocate, disarm before shader restoration can allocate.
        failNextAllocation = false;
        return false; // Rendering after preparation has no window work.
    }
    RHI::ICommandList* AcquireCommandListNative() override { return new Commands; }
    void DiscardCommandListNative(RHI::ICommandList* commands) override { delete static_cast<Commands*>(commands); }
    bool SubmitNative(RHI::ICommandList** commands, uint32_t count) override {
        if(failure == "submit-false") return false;
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
    uint64_t GetCompletedSubmissionNative() override { return completed; }
    uint64_t GetLastSubmissionNative() const override { return lastSubmission; }
    RHI::ShaderHandle CreateShaderNative(const RHI::ShaderDesc& desc) override {
        auto* shader = NullDevice::CreateShaderNative(desc); if(shader) ++shadersLive; return shader;
    }
    void DestroyShaderNative(RHI::ShaderHandle shader) override { --shadersLive; NullDevice::DestroyShaderNative(shader); }
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
    bool UpdateBufferNative(RHI::ICommandList&, RHI::BufferHandle, uint32_t, const void*, uint32_t) override { return true; }
    RHI::ResourceSetHandle CreateResourceSetNative(const RHI::ResourceSetDesc& desc) override {
        if(failure == "set-throw") throw std::bad_alloc();
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
void Run(const std::string& failure) {
    Device device;
    auto renderer = dyf::Renderer::Create(device, reinterpret_cast<void*>(1)); Check(bool(renderer), "renderer creation failed");
    dyf::ModelScene scene;
    dyf::MeshData triangle;
    triangle.vertices.resize(3); triangle.indices = {0,1,2};
    triangle.vertices[1].position.x = 1.f; triangle.vertices[2].position.y = 1.f;
    Check(bool(scene.Add(triangle)), "scene creation failed");
    scene.m_meshSkinInfluences.resize(1);
    scene.m_meshSkinInfluences[0].resize(3);
    for(auto& influence : scene.m_meshSkinInfluences[0]) influence.weights.x = 1.f;
    scene.m_entitySkinPaletteOffsets = {0}; scene.m_jointPaletteMatrices.resize(1);
    const auto baseline = device.GetResourceAllocationCounters().buffers.live;
    {
        dyf::ModelRenderer model(*renderer);
        const uint8_t bytes[] = {1};
        dyf::ModelShaderDesc shaders;
        shaders.vertex = {RHI::ShaderStage::Vertex, "main", bytes, sizeof(bytes)};
        shaders.shadowVertex = shaders.vertex;
        shaders.skinningCompute = {RHI::ShaderStage::Compute, "main", bytes, sizeof(bytes)};
        Check(model.SetShaders(shaders), "model shaders rejected");
        Check(model.SetSkinningExecutionMode(dyf::SkinningExecutionMode::ComputePreSkin), "compute mode rejected");
        device.failure = failure;
        if(failure == "sample-allocation") {
            device.armAfterSubmit = device.delayCompletion = true;
            bool rejected = false;
            for(unsigned attempt = 0; attempt < 64 && !rejected; ++attempt) {
                rejected = !model.Render(scene);
                failNextAllocation = false;
            }
            Check(rejected, "bounded sample handoff allocation was not exercised");
            Check(device.m_userCommands.empty(), "post-submit exception retained user commands");
            Check(device.queriesLive == model.m_gpuSamples.size() + 1, "in-flight query released before completion");
            device.Complete();
            Check(device.queriesLive == model.m_gpuSamples.size(), "failed sample handoff leaked completed query");
            Check(!model.m_meshBuffers.empty() && model.m_meshBuffers[0].buffer, "submitted cache was discarded");
            device.armAfterSubmit = device.delayCompletion = false;
            device.failure.clear();
        } else if(!failure.empty()) {
            Check(!model.Render(scene), "injected failure was ignored");
            Check(device.GetResourceAllocationCounters().buffers.live == baseline, "failed preparation retained buffers");
            Check(device.m_recordedCommands.empty() && device.m_userCommands.empty(), "failed preparation leaked a command list");
            Check(device.setsLive == 0 && device.queriesLive == 0, "failed preparation retained set/query");
            if(failure == "pipeline-throw") Check(device.shadersLive == 0, "temporary compute shader leaked");
            device.failure.clear();
        }
        Check(model.Render(scene), "same-scene retry rejected unsubmitted cache state");
        Check(model.Render(scene), "normal cached preparation failed");
        Check(device.m_userCommands.empty(), "successful preparation leaked user commands");
    }
    Check(device.GetResourceAllocationCounters().buffers.live == baseline, "model destruction retained buffers");
    Check(device.setsLive == 0 && device.queriesLive == 0 && device.shadersLive == 0, "model destruction retained owned handles");
    Check(device.m_recordedCommands.empty(), "model destruction retained commands");
}
}
int main(int argc, char** argv) {
    try {
        const std::string selected = argc > 1 ? argv[1] : "all";
        for(const std::string failure : {"", "pipeline-throw", "buffer-throw", "buffer-null", "query-throw", "set-throw", "set-null", "submit-false", "sample-allocation"}) {
            if(selected != "all" && selected != failure) continue;
            Run(failure); std::cout << "PASS " << (failure.empty() ? "normal" : failure) << '\n';
        }
        return 0;
    } catch(const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
