#include <dyf/Extends/Model/ModelRenderer.h>
#include <dyf/Extends/Model/ModelScene.h>
#include "ModelShaderAssets.h"
#include "dyf/Image.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/ResourceSet.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>

namespace dyf
{
namespace
{
template<class Cleanup>
struct ScopeExit
{
    Cleanup cleanup;
    ~ScopeExit() { cleanup(); }
};
template<class Cleanup> ScopeExit(Cleanup) -> ScopeExit<Cleanup>;

// 기본 Renderer의 정점 입력에 맞춰 변형 결과를 전달한다. Model 자료형은 기본 API로 전달하지 않는다.
struct ModelVertex
{
    float px, py, pz, nx, ny, nz, u, v, tx, ty, tz, tw;
};

std::vector<ModelVertex> BuildVertices(const MeshData& mesh)
{
    std::vector<ModelVertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for(const auto& vertex : mesh.vertices)
        vertices.push_back({vertex.position.x, vertex.position.y, vertex.position.z,
            vertex.normal.x, vertex.normal.y, vertex.normal.z, vertex.uv.x, vertex.uv.y,
            vertex.tangent.x, vertex.tangent.y, vertex.tangent.z, vertex.tangent.w});
    return vertices;
}

bool Failure(const char* message)
{
    std::fprintf(stderr, "dyf: Model renderer: %s\n", message);
    return false;
}

bool SameShader(const RHI::ShaderDesc& left, const RHI::ShaderDesc& right)
{
    return left.stage == right.stage && left.binarySize == right.binarySize &&
        left.entryPoint && right.entryPoint && std::strcmp(left.entryPoint, right.entryPoint) == 0 &&
        left.binary && right.binary && std::memcmp(left.binary, right.binary, left.binarySize) == 0;
}
}

ModelRenderer::ModelRenderer(Renderer& renderer) : m_renderer(renderer) {}

ModelRenderer::~ModelRenderer()
{
    (void)RestoreShaders();
    auto& device = m_renderer.GetDevice();
    ReleaseFrameBuffers();
    ReleaseMeshBuffers();
    for(const auto& sample : m_gpuSamples) device.DestroyTimestampQuery(sample.query);
    if(m_computePipeline) device.DestroyPipeline(m_computePipeline);
    if(m_computeShader) device.DestroyShader(m_computeShader);
}

bool ModelRenderer::SetSkinningExecutionMode(SkinningExecutionMode mode)
{
    if(mode > SkinningExecutionMode::ComputePreSkin) return Failure("Unknown skinning execution mode.");
    if(mode == SkinningExecutionMode::ComputePreSkin && !m_renderer.GetDevice().Supports(RHI::Feature::Compute))
        return Failure("Compute skinning is not supported by this device.");
    m_mode = mode;
    if(mode == SkinningExecutionMode::VertexShader) m_gpuMilliseconds = -1;
    return true;
}

bool ModelRenderer::SetShaders(const ModelShaderDesc& desc)
{
    try
    {
        const std::array<RHI::ShaderDesc, 3> inputs = {desc.vertex, desc.shadowVertex, desc.skinningCompute};
        const std::array<RHI::ShaderStage, 3> stages = {
            RHI::ShaderStage::Vertex, RHI::ShaderStage::Vertex, RHI::ShaderStage::Compute};
        std::array<std::vector<uint8_t>, 3> bytes;
        std::array<std::string, 3> entries;
        for(uint32_t index = 0; index < inputs.size(); ++index)
        {
            const auto& shader = inputs[index];
            if(!shader.binary && !shader.binarySize && !shader.entryPoint) continue;
            if(shader.stage != stages[index] || !shader.binary || !shader.binarySize ||
                !shader.entryPoint || !*shader.entryPoint)
                return Failure("A model shader requires the matching stage, bytes and an entry point.");
            const auto* begin = static_cast<const uint8_t*>(shader.binary);
            bytes[index].assign(begin, begin + shader.binarySize);
            entries[index] = shader.entryPoint;
        }
        m_shaderBytes = std::move(bytes);
        m_shaderEntries = std::move(entries);
        m_computeShaderChanged = true;
        return true;
    }
    catch(const std::exception& error) { return Failure(error.what()); }
}

RHI::ShaderDesc ModelRenderer::ShaderDescription(uint32_t slot, const RHI::ShaderDesc& stock) const
{
    if(m_shaderBytes[slot].empty()) return stock;
    return {slot == 2 ? RHI::ShaderStage::Compute : RHI::ShaderStage::Vertex,
        m_shaderEntries[slot].c_str(), m_shaderBytes[slot].data(), m_shaderBytes[slot].size()};
}

bool ModelRenderer::ConfigureShaders()
{
    auto shaders = m_renderer.GetShaders();
    // 매 호출 시점의 사용자 설정을 복사한다. 변환이 끝나기 전에 이전 복사본을 덮어쓰지 않는다.
    const std::array<RHI::ShaderDesc, 2> original = {shaders.meshVertex, shaders.shadowVertex};
    std::array<std::vector<uint8_t>, 2> originalBytes;
    std::array<std::string, 2> originalEntries;
    auto originalBindings = shaders.vertexBindings;
    for(uint32_t index = 0; index < original.size(); ++index)
    {
        const auto& shader = original[index];
        if(!shader.binarySize) continue;
        const auto* begin = static_cast<const uint8_t*>(shader.binary);
        originalBytes[index].assign(begin, begin + shader.binarySize);
        originalEntries[index] = shader.entryPoint;
    }
    m_originalVertexBytes = std::move(originalBytes);
    m_originalVertexEntries = std::move(originalEntries);
    m_originalVertexBindings = std::move(originalBindings);
    m_originalConstantBytes = shaders.additionalConstantBytes;
    m_configured = true;
    const auto vertex = ShaderDescription(0, GetModelVertexShader(m_renderer.GetLighting().enabled && m_renderer.GetLighting().shadows));
    const auto shadow = ShaderDescription(1, GetModelShadowShader());
    const std::vector<RHI::ResourceBindingLayout> bindings = {
        {11, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Vertex, {}},
        {12, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Vertex, {}}};
    bool matchingBindings = shaders.vertexBindings.size() == bindings.size();
    for(uint32_t index = 0; matchingBindings && index < bindings.size(); ++index)
    {
        const auto& actual = shaders.vertexBindings[index];
        const auto& expected = bindings[index];
        matchingBindings = actual.binding == expected.binding && actual.type == expected.type &&
            actual.count == expected.count && actual.stages == expected.stages;
    }
    if(matchingBindings && shaders.additionalConstantBytes == 16 &&
        SameShader(shaders.meshVertex, vertex) && SameShader(shaders.shadowVertex, shadow)) return true;

    // 모델 단계만 갱신한다. 사용자가 선택한 Fragment, Canvas, ToneMap 단계는 유지한다.
    shaders.meshVertex = vertex;
    shaders.shadowVertex = shadow;
    shaders.vertexBindings = bindings;
    shaders.additionalConstantBytes = 16;
    return m_renderer.SetShaders(shaders);
}

bool ModelRenderer::RestoreShaders()
{
    if(!m_configured) return true;
    try
    {
        // Fragment, Canvas, ToneMap의 현재 설정을 보존하고 이번 호출이 교체한 정점 입력만 복원한다.
        auto shaders = m_renderer.GetShaders();
        const auto original = [&](uint32_t slot) -> RHI::ShaderDesc
        {
            if(m_originalVertexBytes[slot].empty()) return {};
            return {RHI::ShaderStage::Vertex, m_originalVertexEntries[slot].c_str(),
                m_originalVertexBytes[slot].data(), m_originalVertexBytes[slot].size()};
        };
        shaders.meshVertex = original(0);
        shaders.shadowVertex = original(1);
        shaders.vertexBindings = m_originalVertexBindings;
        shaders.additionalConstantBytes = m_originalConstantBytes;
        if(!m_renderer.SetShaders(shaders)) return false;
        m_configured = false;
        return true;
    }
    catch(const std::exception& error) { return Failure(error.what()); }
    catch(...) { return Failure("Shader restoration failed."); }
}

bool ModelRenderer::PrepareComputePipeline()
{
    if(m_computePipeline && !m_computeShaderChanged) return true;
    auto& device = m_renderer.GetDevice();
    auto* shader = device.CreateShader(ShaderDescription(2, GetModelComputeShader()));
    if(!shader) return Failure("Compute shader creation failed.");
    ScopeExit discardShader{[&] { if(shader) device.DestroyShader(shader); }};
    const std::array<RHI::ResourceBindingLayout, 4> bindings = {{
        {11, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Compute, {}},
        {12, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Compute, {}},
        {14, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Compute, {}},
        {15, RHI::ResourceBindingType::ReadWriteStorageBuffer, 1, RHI::ShaderStageFlags::Compute, {}}
    }};
    auto* pipeline = device.CreateComputePipeline({shader,
        {bindings.data(), static_cast<uint32_t>(bindings.size()), 16, RHI::ShaderStageFlags::Compute, 10}});
    if(!pipeline) return Failure("Compute pipeline creation failed.");
    ScopeExit discardPipeline{[&] { if(pipeline) device.DestroyPipeline(pipeline); }};
    if(m_computePipeline) device.DestroyPipeline(m_computePipeline);
    if(m_computeShader) device.DestroyShader(m_computeShader);
    m_computeShader = shader;
    m_computePipeline = pipeline;
    shader = nullptr;
    pipeline = nullptr;
    m_computeShaderChanged = false;
    return true;
}

void ModelRenderer::ReleaseFrameBuffers()
{
    m_draws.clear();
    for(auto& frame : m_frames) ReleaseFrame(frame);
    m_frames.clear();
    if(m_influenceBuffer) m_renderer.GetDevice().DestroyBuffer(m_influenceBuffer);
    m_influenceBuffer = nullptr;
    m_influences.clear();
    m_influenceOffsets.clear();
    m_influenceCounts.clear(); m_requiredInfluenceJoints.clear(); m_validInfluences.clear();
}

void ModelRenderer::ReleaseFrame(FrameResources& frame)
{
    auto& device = m_renderer.GetDevice();
    for(auto* set : frame.sets) if(set) device.DestroyResourceSet(set);
    for(auto& buffer : frame.buffers) if(buffer.buffer) device.DestroyBuffer(buffer.buffer);
    frame = {};
}

void ModelRenderer::ReleaseMeshBuffers()
{
    for(auto& mesh : m_meshBuffers)
        if(mesh.buffer) m_renderer.GetDevice().DestroyBuffer(mesh.buffer);
    m_meshBuffers.clear();
}

void ModelRenderer::CollectGpuSamples()
{
    auto& device = m_renderer.GetDevice();
    while(!m_gpuSamples.empty() && device.IsComplete(m_gpuSamples.front().completion))
    {
        const auto sample = m_gpuSamples.front();
        uint64_t ticks[2];
        if(device.ReadTimestamps(sample.query, 0, 2, ticks) && m_mode == SkinningExecutionMode::ComputePreSkin)
        {
            const uint32_t bits = device.GetTimestampValidBits();
            const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
            m_gpuMilliseconds = ((ticks[1] - ticks[0]) & mask) * device.GetTimestampPeriodNanoseconds() / 1e6;
        }
        device.DestroyTimestampQuery(sample.query);
        m_gpuSamples.pop_front();
    }
}

bool ModelRenderer::Prepare(const ModelScene& scene)
{
    static_assert(sizeof(ModelVertex) == 48);
    static_assert(sizeof(SkinInfluence) == 48 && offsetof(SkinInfluence, dqBlendWeight) == 32);
    static_assert(sizeof(SkinJointMatrices) == 176);
    auto& device = m_renderer.GetDevice();
    const bool compute = m_mode == SkinningExecutionMode::ComputePreSkin;
    if(compute && !PrepareComputePipeline()) return false;

    // Compare actual influence bytes, including edits made without changing mesh identity.
    // Packing and immutable validation are needed only when those bytes/counts change.
    bool sameInfluences = m_influenceBuffer && m_influenceOffsets.size() == scene.Meshes().size();
    for(uint32_t index = 0; sameInfluences && index < scene.Meshes().size(); ++index)
    {
        const auto& source = scene.GetMeshSkinInfluences(static_cast<MeshID>(index));
        const uint32_t offset = m_influenceOffsets[index];
        if(source.empty()) { sameInfluences = offset == UINT32_MAX; continue; }
        if(source.size() != scene.Meshes()[index]->vertices.size() || offset == UINT32_MAX ||
            uint64_t(offset) + source.size() > m_influences.size()) { sameInfluences = false; break; }
        sameInfluences = m_influenceCounts[index] == source.size() &&
            std::memcmp(m_influences.data() + offset, source.data(), source.size() * sizeof(SkinInfluence)) == 0;
    }
    std::vector<SkinInfluence> influences;
    std::vector<uint32_t> offsets, counts;
    std::vector<uint64_t> requiredJoints;
    std::vector<bool> validInfluences;
    if(!sameInfluences)
    {
        offsets.assign(scene.Meshes().size(), UINT32_MAX);
        counts.assign(scene.Meshes().size(), 0);
        requiredJoints.assign(scene.Meshes().size(), 0);
        validInfluences.assign(scene.Meshes().size(), true);
        for(uint32_t index = 0; index < scene.Meshes().size(); ++index)
        {
            const auto& source = scene.GetMeshSkinInfluences(static_cast<MeshID>(index));
            if(source.empty()) continue;
            if(source.size() != scene.Meshes()[index]->vertices.size() ||
                influences.size() + source.size() > UINT32_MAX / sizeof(SkinInfluence))
                return Failure("Skin influences do not match the model vertices.");
            offsets[index] = static_cast<uint32_t>(influences.size());
            counts[index] = static_cast<uint32_t>(source.size());
            for(const auto& influence : source)
            {
                if(!std::isfinite(influence.dqBlendWeight) || influence.dqBlendWeight < 0 || influence.dqBlendWeight > 1)
                    validInfluences[index] = false;
                for(uint32_t component = 0; component < 4; ++component)
                {
                    const float weight = influence.weights[component];
                    if(!std::isfinite(weight) || weight < 0)
                        validInfluences[index] = false;
                    if(weight > 0)
                    {
                        const uint32_t joint = influence.jointIndices[component];
                        requiredJoints[index] = std::max(requiredJoints[index], uint64_t(joint) + 1);
                    }
                }
            }
            influences.insert(influences.end(), source.begin(), source.end());
        }
        if(influences.empty()) influences.emplace_back();
    }
    const auto& activeOffsets = sameInfluences ? m_influenceOffsets : offsets;
    const auto& activeRequiredJoints = sameInfluences ? m_requiredInfluenceJoints : requiredJoints;
    const auto& activeValid = sameInfluences ? m_validInfluences : validInfluences;
    const auto& palette = scene.JointPaletteMatrices();
    if(palette.size() > UINT32_MAX / sizeof(SkinJointMatrices)) return Failure("Skin palette is too large.");
    for(uint32_t index = 0; index < scene.GetEntityCount(); ++index)
    {
        const auto entity = static_cast<EntityID>(index);
        const uint32_t paletteOffset = scene.GetEntitySkinPaletteOffset(entity);
        if(paletteOffset == UINT32_MAX) continue;
        const auto mesh = scene.GetEntityMesh(entity);
        if(!IsValid(mesh) || ToIndex(mesh) >= activeOffsets.size() || activeOffsets[ToIndex(mesh)] == UINT32_MAX)
            return Failure("A skinned entity requires mesh influences.");
        const uint64_t required = activeRequiredJoints[ToIndex(mesh)];
        if(!activeValid[ToIndex(mesh)] || (required && uint64_t(paletteOffset) + required > palette.size()))
            return Failure("Skin influence references an invalid joint or weight.");
    }

    const uint32_t limit = std::max(1u, device.GetDesc().maxFramesInFlight);
    uint32_t selected = 0;
    for(; selected < m_frames.size(); ++selected)
        if(!m_frames[selected].completion || device.IsComplete(m_frames[selected].completion)) break;
    if(selected == m_frames.size())
    {
        if(m_frames.size() < limit) m_frames.emplace_back();
        else
        {
            selected = (m_frameIndex + 1) % static_cast<uint32_t>(m_frames.size());
            if(!device.Wait(m_frames[selected].completion, UINT64_MAX))
                return Failure("Model frame completion wait failed.");
        }
    }
    m_frameIndex = selected;
    auto& frame = m_frames[selected];
    m_draws.clear();
    m_draws.resize(scene.GetEntityCount());
    bool submitted = false;
    ScopeExit rollback{[&] {
        // Recorded transitions must never survive a rejected submission as usable state.
        if(!submitted)
        {
            m_draws.clear();
            ReleaseFrame(frame);
            ReleaseMeshBuffers();
            if(m_influenceBuffer) device.DestroyBuffer(m_influenceBuffer);
            m_influenceBuffer = nullptr;
            m_influences.clear(); m_influenceOffsets.clear(); m_influenceCounts.clear(); m_requiredInfluenceJoints.clear(); m_validInfluences.clear();
        }
    }};
    RHI::ICommandList* commands = nullptr;
    ScopeExit discardCommands{[&] { if(commands) device.DestroyCommandList(commands); }};
    bool prepared = true;
    auto acquire = [&] {
        if(!commands) commands = device.AcquireCommandList();
        if(!commands) prepared = false;
        return commands != nullptr;
    };
    const auto destroyBuffer = [&device](RHI::Buffer* buffer) { device.DestroyBuffer(buffer); };
    auto createUpload = [&](const void* data, size_t count, uint32_t stride,
        RHI::BufferUsage usage, RHI::ResourceState state) -> RHI::BufferHandle
    {
        if(!count || count > UINT32_MAX / stride || !acquire()) { prepared = false; return nullptr; }
        std::unique_ptr<RHI::Buffer, decltype(destroyBuffer)> owned(
            device.CreateBuffer({static_cast<uint32_t>(count * stride), stride, usage, RHI::ResourceState::CopyDestination}), destroyBuffer);
        if(!owned) { prepared = false; return nullptr; }
        if(!device.UpdateBuffer(*commands, owned.get(), 0, data, static_cast<uint32_t>(count * stride)))
            prepared = false;
        const RHI::ResourceBarrierDesc ready{owned.get(), nullptr, RHI::ResourceState::CopyDestination, state, {}};
        commands->ResourceBarrier(&ready, 1);
        return owned.release();
    };
    if(!sameInfluences)
    {
        auto* buffer = createUpload(influences.data(), influences.size(), sizeof(SkinInfluence),
            RHI::BufferUsage::Storage, RHI::ResourceState::ShaderResource);
        if(!buffer) return Failure("Skin influence upload failed.");
        if(m_influenceBuffer) device.DestroyBuffer(m_influenceBuffer);
        m_influenceBuffer = buffer;
        m_influences = std::move(influences); m_influenceOffsets = std::move(offsets);
        m_influenceCounts = std::move(counts); m_requiredInfluenceJoints = std::move(requiredJoints);
        m_validInfluences = std::move(validInfluences);
    }
    // activeOffsets may refer to a moved local vector; all subsequent reads use the published cache.
    auto* influenceBuffer = m_influenceBuffer;
    // Completed unused slots must not pin buffers from historical scenes indefinitely.
    // Pointer identity is only a retirement key; uploads/validation use the actual data above.
    std::vector<uintptr_t> workingSet = {uintptr_t(compute), reinterpret_cast<uintptr_t>(influenceBuffer),
        compute ? reinterpret_cast<uintptr_t>(m_computePipeline) : 0,
        scene.Meshes().size(), scene.GetEntityCount(), std::max(size_t{1}, palette.size())};
    for(const auto& mesh : scene.Meshes())
    {
        workingSet.push_back(reinterpret_cast<uintptr_t>(mesh.get()));
        workingSet.push_back(mesh->vertices.size());
    }
    for(uint32_t index = 0; index < scene.GetEntityCount(); ++index)
    {
        const auto entity = static_cast<EntityID>(index);
        const auto* morphed = scene.TryGetEntityMorphedMesh(entity);
        workingSet.push_back(ToIndex(scene.GetEntityMesh(entity)));
        workingSet.push_back(morphed ? morphed->vertices.size() : 0);
        workingSet.push_back(scene.GetEntitySkinPaletteOffset(entity) != UINT32_MAX);
    }
    for(uint32_t index = 0; index < m_frames.size(); ++index)
        if(index != selected && m_frames[index].workingSet != workingSet &&
            (!m_frames[index].completion || device.IsComplete(m_frames[index].completion)))
            ReleaseFrame(m_frames[index]);
    frame.workingSet = std::move(workingSet);
    size_t cursor = 0;
    auto bufferSlot = [&](uint32_t size, uint32_t stride, RHI::BufferUsage usage,
        RHI::ResourceState state) -> FrameBuffer* {
        if(cursor == frame.buffers.size()) frame.buffers.emplace_back();
        auto& cached = frame.buffers[cursor++];
        if(cached.buffer && (cached.buffer->GetDesc().size != size || cached.buffer->GetDesc().stride != stride ||
            cached.buffer->GetDesc().usage != usage))
        { device.DestroyBuffer(cached.buffer); cached = {}; }
        if(!cached.buffer)
        {
            cached.buffer = device.CreateBuffer({size, stride, usage, state});
            cached.state = state;
        }
        if(!cached.buffer) { prepared = false; return nullptr; }
        return &cached;
    };
    auto upload = [&](const void* data, size_t count, uint32_t stride,
        RHI::BufferUsage usage, RHI::ResourceState state) -> RHI::BufferHandle {
        if(!count || count > UINT32_MAX / stride) { prepared = false; return nullptr; }
        const uint32_t bytes = static_cast<uint32_t>(count * stride);
        auto* cached = bufferSlot(bytes, stride, usage, RHI::ResourceState::CopyDestination);
        if(!cached) return nullptr;
        if(cached->bytes.size() == bytes && std::memcmp(cached->bytes.data(), data, bytes) == 0)
            return cached->buffer;
        if(!acquire()) return nullptr;
        if(cached->state != RHI::ResourceState::CopyDestination)
        {
            const RHI::ResourceBarrierDesc before{cached->buffer, nullptr, cached->state, RHI::ResourceState::CopyDestination, {}};
            commands->ResourceBarrier(&before, 1);
        }
        if(!device.UpdateBuffer(*commands, cached->buffer, 0, data, bytes)) prepared = false;
        const RHI::ResourceBarrierDesc ready{cached->buffer, nullptr, RHI::ResourceState::CopyDestination, state, {}};
        commands->ResourceBarrier(&ready, 1);
        const auto* first = static_cast<const uint8_t*>(data);
        cached->bytes.assign(first, first + bytes); cached->state = state;
        return cached->buffer;
    };
    const SkinJointMatrices identity;
    auto* paletteBuffer = upload(palette.empty() ? &identity : palette.data(), palette.empty() ? 1 : palette.size(),
        sizeof(SkinJointMatrices), RHI::BufferUsage::Storage, RHI::ResourceState::ShaderResource);
    if(!paletteBuffer) return Failure("Skin palette upload failed.");
    if(m_meshBuffers.size() != scene.Meshes().size()) { ReleaseMeshBuffers(); m_meshBuffers.resize(scene.Meshes().size()); }
    std::vector<bool> meshUsed(m_meshBuffers.size(), false);
    for(uint32_t index = 0; prepared && index < scene.GetEntityCount(); ++index)
    {
        const auto entity = static_cast<EntityID>(index);
        auto& draw = m_draws[index];
        const uint32_t paletteOffset = scene.GetEntitySkinPaletteOffset(entity);
        const auto mesh = scene.GetEntityMesh(entity);
        const uint32_t meshIndex = ToIndex(mesh);
        if(const auto* morphed = scene.TryGetEntityMorphedMesh(entity))
        {
            const auto vertices = BuildVertices(*morphed);
            draw.vertexBuffer = upload(vertices.data(), vertices.size(), sizeof(ModelVertex),
                RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage, RHI::ResourceState::VertexBuffer);
        }
        if(compute && paletteOffset != UINT32_MAX && !draw.vertexBuffer)
        {
            meshUsed[meshIndex] = true;
            auto& cached = m_meshBuffers[meshIndex];
            const auto& source = scene.Meshes()[meshIndex];
            if(cached.source.lock() != source)
            {
                if(cached.buffer) device.DestroyBuffer(cached.buffer);
                cached = {};
                const auto vertices = BuildVertices(*source);
                cached.buffer = createUpload(vertices.data(), vertices.size(), sizeof(ModelVertex),
                    RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage, RHI::ResourceState::VertexBuffer);
                cached.source = source;
            }
            draw.vertexBuffer = cached.buffer;
        }
        draw.vertexResources = {
            {11, 0, influenceBuffer, nullptr, 0, influenceBuffer->GetDesc().size, {}},
            {12, 0, paletteBuffer, nullptr, 0, paletteBuffer->GetDesc().size, {}}};
        const std::array<uint32_t, 4> constants = {
            compute || !IsValid(mesh) || meshIndex >= m_influenceOffsets.size() ? UINT32_MAX : m_influenceOffsets[meshIndex], paletteOffset, 0, 0};
        draw.inlineConstants.resize(sizeof(constants));
        std::memcpy(draw.inlineConstants.data(), constants.data(), sizeof(constants));
    }
    RHI::TimestampQueryHandle query = nullptr;
    ScopeExit discardQuery{[&] { if(query) device.DestroyTimestampQuery(query); }};
    if(frame.sets.size() < scene.GetEntityCount()) frame.sets.resize(scene.GetEntityCount(), nullptr);
    for(size_t index = 0; index < m_meshBuffers.size(); ++index)
        if(!meshUsed[index])
        {
            if(m_meshBuffers[index].buffer) device.DestroyBuffer(m_meshBuffers[index].buffer);
            m_meshBuffers[index] = {};
        }
    if(compute && prepared && acquire())
    {
        if(device.Supports(RHI::Feature::TimestampQuery)) query = device.CreateTimestampQuery({2});
        if(query) { commands->ResetTimestamps(query, 0, 2); commands->WriteTimestamp(query, 0); }
        commands->BindComputePipeline(m_computePipeline);
        for(uint32_t index = 0; index < scene.GetEntityCount(); ++index)
        {
            const auto entity = static_cast<EntityID>(index);
            const uint32_t paletteOffset = scene.GetEntitySkinPaletteOffset(entity);
            if(paletteOffset == UINT32_MAX)
            {
                if(frame.sets[index]) device.DestroyResourceSet(frame.sets[index]);
                frame.sets[index] = nullptr;
                continue;
            }
            auto& draw = m_draws[index];
            auto* source = draw.vertexBuffer;
            if(!source) { prepared = false; break; }
            const uint32_t bytes = source->GetDesc().size;
            auto* cached = bufferSlot(bytes, sizeof(ModelVertex), RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage,
                RHI::ResourceState::UnorderedAccess);
            if(!cached) break;
            auto* output = cached->buffer;
            // Compute writes invalidate any CPU upload snapshot from a prior slot role.
            cached->bytes.clear();
            const std::array<RHI::ResourceBinding, 4> bindings = {{
                {11, 0, influenceBuffer, nullptr, 0, influenceBuffer->GetDesc().size, {}},
                {12, 0, paletteBuffer, nullptr, 0, paletteBuffer->GetDesc().size, {}},
                {14, 0, source, nullptr, 0, bytes, {}}, {15, 0, output, nullptr, 0, bytes, {}}
            }};
            auto& set = frame.sets[index];
            bool matching = set && set->GetPipeline() == m_computePipeline && set->GetBindingCount() == bindings.size();
            for(uint32_t binding = 0; matching && binding < bindings.size(); ++binding)
                matching = set->GetBindings()[binding].buffer == bindings[binding].buffer &&
                    set->GetBindings()[binding].size == bindings[binding].size;
            if(!matching)
            {
                if(set) device.DestroyResourceSet(set);
                set = nullptr;
                set = device.CreateResourceSet({m_computePipeline, bindings.data(), static_cast<uint32_t>(bindings.size())});
                if(!set) { prepared = false; break; }
            }
            const RHI::ResourceBarrierDesc before{source, nullptr, RHI::ResourceState::VertexBuffer, RHI::ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&before, 1);
            if(cached->state != RHI::ResourceState::UnorderedAccess)
            {
                const RHI::ResourceBarrierDesc ready{output, nullptr, cached->state, RHI::ResourceState::UnorderedAccess, {}};
                commands->ResourceBarrier(&ready, 1);
            }
            commands->BindResourceSet(set);
            const uint32_t count = bytes / sizeof(ModelVertex);
            const std::array<uint32_t, 4> constants = {count, m_influenceOffsets[ToIndex(scene.GetEntityMesh(entity))], paletteOffset, 0};
            commands->SetInlineConstants(0, sizeof(constants), constants.data());
            commands->Dispatch((count + 63) / 64, 1, 1);
            const std::array<RHI::ResourceBarrierDesc, 2> after = {{
                {source, nullptr, RHI::ResourceState::ShaderResource, RHI::ResourceState::VertexBuffer, {}},
                {output, nullptr, RHI::ResourceState::UnorderedAccess, RHI::ResourceState::VertexBuffer, {}}
            }};
            commands->ResourceBarrier(after.data(), static_cast<uint32_t>(after.size()));
            cached->state = RHI::ResourceState::VertexBuffer;
            draw.vertexBuffer = output;
        }
        if(query) commands->WriteTimestamp(query, 1);
    }
    // Shrink completed-slot storage to this scene's working set (no historical entity cache).
    for(size_t index = cursor; index < frame.buffers.size(); ++index) device.DestroyBuffer(frame.buffers[index].buffer);
    frame.buffers.resize(cursor);
    for(size_t index = scene.GetEntityCount(); index < frame.sets.size(); ++index)
        if(frame.sets[index]) device.DestroyResourceSet(frame.sets[index]);
    frame.sets.resize(scene.GetEntityCount());
    if(!compute) for(auto& set : frame.sets) { if(set) device.DestroyResourceSet(set); set = nullptr; }
    if(!commands) { submitted = prepared; return prepared; }
    const bool closed = commands->Close();
    RHI::FenceHandle completion;
    submitted = prepared && closed && device.Submit({&commands, 1, nullptr, 0}, completion);
    // Publish ownership before an allocation in the timestamp handoff can throw.
    if(submitted) frame.completion = completion;
    if(query && submitted) { m_gpuSamples.push_back({query, completion}); query = nullptr; }
    if(!submitted) return Failure("Model deformation upload or compute submission failed.");
    return true;
}

bool ModelRenderer::RenderScene(const ModelScene& scene, const Camera* camera, const Canvas* overlay, Image* readback)
{
    bool rendered = false;
    bool drawAttempted = false;
    RHI::FenceHandle previousDraw;
    ScopeExit retainDrawCompletion{[&] {
        if(!drawAttempted) return;
        const auto completion = m_renderer.GetLastRenderCompletion();
        if(completion && completion != previousDraw)
            m_frames[m_frameIndex].completion = completion;
    }};
    try
    {
        if(readback) *readback = {};
        CollectGpuSamples();
        // 이전 복원 자체가 실패했다면 원래 설정을 덮어쓰지 않고 다시 복원부터 처리한다.
        if(RestoreShaders() && ConfigureShaders() && Prepare(scene))
        {
            previousDraw = m_renderer.GetLastRenderCompletion();
            drawAttempted = true;
            rendered = camera ? m_renderer.Render(scene, *camera, m_draws, overlay, readback) :
                m_renderer.Render(scene, m_draws, overlay, readback);
        }
    }
    catch(const std::exception& error) { (void)Failure(error.what()); }
    catch(...) { (void)Failure("Model rendering failed."); }
    // 성공, 준비 실패, 예외 경로 모두 복원을 예약한 후 호출자에게 돌아간다.
    const bool restored = RestoreShaders();
    return rendered && restored;
}

bool ModelRenderer::Render(const ModelScene& scene, Image* readback)
{ return RenderScene(scene, nullptr, nullptr, readback); }
bool ModelRenderer::Render(const ModelScene& scene, const Camera& camera, Image* readback)
{ return RenderScene(scene, &camera, nullptr, readback); }
bool ModelRenderer::Render(const ModelScene& scene, const Canvas& overlay, Image* readback)
{ return RenderScene(scene, nullptr, &overlay, readback); }
bool ModelRenderer::Render(const ModelScene& scene, const Camera& camera, const Canvas& overlay, Image* readback)
{ return RenderScene(scene, &camera, &overlay, readback); }
}
