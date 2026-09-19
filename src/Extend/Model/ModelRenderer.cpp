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
    const std::array<RHI::ResourceBindingLayout, 4> bindings = {{
        {11, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Compute, {}},
        {12, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Compute, {}},
        {14, RHI::ResourceBindingType::ReadOnlyStorageBuffer, 1, RHI::ShaderStageFlags::Compute, {}},
        {15, RHI::ResourceBindingType::ReadWriteStorageBuffer, 1, RHI::ShaderStageFlags::Compute, {}}
    }};
    auto* pipeline = device.CreateComputePipeline({shader,
        {bindings.data(), static_cast<uint32_t>(bindings.size()), 16, RHI::ShaderStageFlags::Compute, 10}});
    if(!pipeline) { device.DestroyShader(shader); return Failure("Compute pipeline creation failed."); }
    if(m_computePipeline) device.DestroyPipeline(m_computePipeline);
    if(m_computeShader) device.DestroyShader(m_computeShader);
    m_computeShader = shader;
    m_computePipeline = pipeline;
    m_computeShaderChanged = false;
    return true;
}

void ModelRenderer::ReleaseFrameBuffers()
{
    m_draws.clear();
    for(auto* buffer : m_frameBuffers) m_renderer.GetDevice().DestroyBuffer(buffer);
    m_frameBuffers.clear();
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

    std::vector<SkinInfluence> influences;
    std::vector<uint32_t> offsets(scene.Meshes().size(), UINT32_MAX);
    for(uint32_t index = 0; index < scene.Meshes().size(); ++index)
    {
        const auto& source = scene.GetMeshSkinInfluences(static_cast<MeshID>(index));
        if(source.empty()) continue;
        if(source.size() != scene.Meshes()[index]->vertices.size() ||
            influences.size() + source.size() > UINT32_MAX / sizeof(SkinInfluence))
            return Failure("Skin influences do not match the model vertices.");
        offsets[index] = static_cast<uint32_t>(influences.size());
        influences.insert(influences.end(), source.begin(), source.end());
    }
    if(influences.empty()) influences.emplace_back();
    const auto& palette = scene.JointPaletteMatrices();
    if(palette.size() > UINT32_MAX / sizeof(SkinJointMatrices)) return Failure("Skin palette is too large.");
    for(uint32_t index = 0; index < scene.GetEntityCount(); ++index)
    {
        const auto entity = static_cast<EntityID>(index);
        const uint32_t paletteOffset = scene.GetEntitySkinPaletteOffset(entity);
        if(paletteOffset == UINT32_MAX) continue;
        const auto mesh = scene.GetEntityMesh(entity);
        if(!IsValid(mesh) || ToIndex(mesh) >= offsets.size() || offsets[ToIndex(mesh)] == UINT32_MAX)
            return Failure("A skinned entity requires mesh influences.");
        for(const auto& influence : scene.GetMeshSkinInfluences(mesh))
        {
            if(!std::isfinite(influence.dqBlendWeight) || influence.dqBlendWeight < 0 || influence.dqBlendWeight > 1)
                return Failure("Invalid dual quaternion blend weight.");
            for(uint32_t component = 0; component < 4; ++component)
            {
                const float weight = influence.weights[component];
                if(!std::isfinite(weight) || weight < 0 ||
                    (weight > 0 && uint64_t{paletteOffset} + influence.jointIndices[component] >= palette.size()))
                    return Failure("Skin influence references an invalid joint or weight.");
            }
        }
    }

    ReleaseFrameBuffers();
    if(m_meshBuffers.size() != scene.Meshes().size())
    {
        ReleaseMeshBuffers();
        m_meshBuffers.resize(scene.Meshes().size());
    }
    m_draws.resize(scene.GetEntityCount());
    auto* commands = device.AcquireCommandList();
    if(!commands) return Failure("Model upload command list creation failed.");
    bool prepared = true;
    auto upload = [&](const void* data, size_t count, uint32_t stride,
        RHI::BufferUsage usage, RHI::ResourceState state) -> RHI::BufferHandle
    {
        if(!count || count > UINT32_MAX / stride) { prepared = false; return nullptr; }
        auto* buffer = device.CreateBuffer({static_cast<uint32_t>(count * stride), stride,
            usage, RHI::ResourceState::CopyDestination});
        if(!buffer) { prepared = false; return nullptr; }
        m_frameBuffers.push_back(buffer);
        if(!device.UpdateBuffer(*commands, buffer, 0, data, buffer->GetDesc().size)) prepared = false;
        const RHI::ResourceBarrierDesc ready{buffer, nullptr, RHI::ResourceState::CopyDestination, state, {}};
        commands->ResourceBarrier(&ready, 1);
        return buffer;
    };
    auto* influenceBuffer = upload(influences.data(), influences.size(), sizeof(SkinInfluence),
        RHI::BufferUsage::Storage, RHI::ResourceState::ShaderResource);
    const SkinJointMatrices identity;
    auto* paletteBuffer = upload(palette.empty() ? &identity : palette.data(), palette.empty() ? 1 : palette.size(),
        sizeof(SkinJointMatrices), RHI::BufferUsage::Storage, RHI::ResourceState::ShaderResource);

    // 모프 결과와 Compute 입력을 준비한다. 일반 Mesh 업로드는 기본 Renderer가 계속 담당한다.
    for(uint32_t index = 0; prepared && index < scene.GetEntityCount(); ++index)
    {
        const auto entity = static_cast<EntityID>(index);
        auto& draw = m_draws[index];
        const uint32_t paletteOffset = scene.GetEntitySkinPaletteOffset(entity);
        const auto mesh = scene.GetEntityMesh(entity);
        const uint32_t meshIndex = ToIndex(mesh);
        const auto* morphed = scene.TryGetEntityMorphedMesh(entity);
        if(morphed)
        {
            const auto vertices = BuildVertices(*morphed);
            draw.vertexBuffer = upload(vertices.data(), vertices.size(), sizeof(ModelVertex),
                RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage, RHI::ResourceState::VertexBuffer);
        }
        if(compute && paletteOffset != UINT32_MAX && !draw.vertexBuffer)
        {
            auto& cached = m_meshBuffers[meshIndex];
            const auto& source = scene.Meshes()[meshIndex];
            if(cached.source.lock() != source)
            {
                if(cached.buffer) device.DestroyBuffer(cached.buffer);
                const auto vertices = BuildVertices(*source);
                cached.buffer = upload(vertices.data(), vertices.size(), sizeof(ModelVertex),
                    RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage, RHI::ResourceState::VertexBuffer);
                if(cached.buffer) m_frameBuffers.pop_back();
                cached.source = source;
            }
            draw.vertexBuffer = cached.buffer;
        }
        draw.vertexResources = {
            {11, 0, influenceBuffer, nullptr, 0, influenceBuffer->GetDesc().size, {}},
            {12, 0, paletteBuffer, nullptr, 0, paletteBuffer->GetDesc().size, {}}};
        // 추가 상수는 16바이트로 정렬한다. 뒤의 두 값은 정점 셰이더 자료 배치의 패딩이다.
        const std::array<uint32_t, 4> constants = {
            compute || !IsValid(mesh) || meshIndex >= offsets.size() ? UINT32_MAX : offsets[meshIndex], paletteOffset, 0, 0};
        draw.inlineConstants.resize(sizeof(constants));
        std::memcpy(draw.inlineConstants.data(), constants.data(), sizeof(constants));
    }

    std::vector<RHI::ResourceSetHandle> resourceSets;
    RHI::TimestampQueryHandle query = nullptr;
    if(compute && prepared)
    {
        if(device.Supports(RHI::Feature::TimestampQuery)) query = device.CreateTimestampQuery({2});
        if(query) { commands->ResetTimestamps(query, 0, 2); commands->WriteTimestamp(query, 0); }
        commands->BindComputePipeline(m_computePipeline);
        for(uint32_t index = 0; prepared && index < scene.GetEntityCount(); ++index)
        {
            const auto entity = static_cast<EntityID>(index);
            const uint32_t paletteOffset = scene.GetEntitySkinPaletteOffset(entity);
            if(paletteOffset == UINT32_MAX) continue;
            auto& draw = m_draws[index];
            auto* source = draw.vertexBuffer;
            if(!source) { prepared = false; break; }
            const uint32_t bytes = source->GetDesc().size;
            auto* output = device.CreateBuffer({bytes, sizeof(ModelVertex),
                RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage, RHI::ResourceState::UnorderedAccess});
            if(!output) { prepared = false; break; }
            m_frameBuffers.push_back(output);
            const std::array<RHI::ResourceBinding, 4> bindings = {{
                {11, 0, influenceBuffer, nullptr, 0, influenceBuffer->GetDesc().size, {}},
                {12, 0, paletteBuffer, nullptr, 0, paletteBuffer->GetDesc().size, {}},
                {14, 0, source, nullptr, 0, bytes, {}}, {15, 0, output, nullptr, 0, bytes, {}}
            }};
            auto* set = device.CreateResourceSet({m_computePipeline, bindings.data(), static_cast<uint32_t>(bindings.size())});
            if(!set) { prepared = false; break; }
            resourceSets.push_back(set);
            const RHI::ResourceBarrierDesc before{source, nullptr,
                RHI::ResourceState::VertexBuffer, RHI::ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&before, 1);
            commands->BindResourceSet(set);
            const uint32_t count = bytes / sizeof(ModelVertex);
            const std::array<uint32_t, 4> constants = {
                count, offsets[ToIndex(scene.GetEntityMesh(entity))], paletteOffset, 0};
            commands->SetInlineConstants(0, sizeof(constants), constants.data());
            commands->Dispatch((count + 63) / 64, 1, 1);
            const std::array<RHI::ResourceBarrierDesc, 2> after = {{
                {source, nullptr, RHI::ResourceState::ShaderResource, RHI::ResourceState::VertexBuffer, {}},
                {output, nullptr, RHI::ResourceState::UnorderedAccess, RHI::ResourceState::VertexBuffer, {}}
            }};
            commands->ResourceBarrier(after.data(), static_cast<uint32_t>(after.size()));
            draw.vertexBuffer = output;
        }
        if(query) commands->WriteTimestamp(query, 1);
    }
    const bool closed = commands->Close();
    RHI::FenceHandle completion;
    const bool submitted = prepared && closed && device.Submit({&commands, 1, nullptr, 0}, completion);
    device.DestroyCommandList(commands);
    for(auto* set : resourceSets) device.DestroyResourceSet(set);
    if(query)
    {
        if(submitted) m_gpuSamples.push_back({query, completion});
        else device.DestroyTimestampQuery(query);
    }
    if(!submitted)
    {
        ReleaseFrameBuffers();
        ReleaseMeshBuffers();
        return Failure("Model deformation upload or compute submission failed.");
    }
    return true;
}

bool ModelRenderer::RenderScene(const ModelScene& scene, const Camera* camera, const Canvas* overlay, Image* readback)
{
    bool rendered = false;
    try
    {
        if(readback) *readback = {};
        CollectGpuSamples();
        // 이전 복원 자체가 실패했다면 원래 설정을 덮어쓰지 않고 다시 복원부터 처리한다.
        if(RestoreShaders() && ConfigureShaders() && Prepare(scene))
            rendered = camera ? m_renderer.Render(scene, *camera, m_draws, overlay, readback) :
                m_renderer.Render(scene, m_draws, overlay, readback);
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
