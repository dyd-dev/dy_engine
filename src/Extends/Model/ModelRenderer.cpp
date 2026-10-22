#include "../../Platform/Profiler/CollectorInternal.h"
#include <dyf/Extends/Model/ModelRenderer.h>
#include <dyf/Extends/Model/ModelScene.h>
#include "ModelShaderAssets.h"
#include "ModelSkinning.h"
#include "../../Platform/Profiler/GpuTimestamp.h"
#include "dyf/Image.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Binding.h"
#include <algorithm>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>

namespace dyf
{
struct ModelRenderer::ProfilingState
{
    struct GpuSample
    {
        RHI::TimestampQueryHandle query = nullptr;
        RHI::FenceHandle completion;
        Platform::LiveGpuSpan liveSpan;
        std::optional<RHI::TimestampCalibration> calibration;
        std::uint64_t submittedNs=0;
    };
    Platform::LiveId id=0;
    Platform::ControlRegistration skinningControl;
    std::array<Platform::ControlRegistration,5> optimizationControls;
    std::array<Platform::LiveId,5> optimizationRequests{};
    std::array<Platform::ControlRegistration,4> catalog;
    std::deque<GpuSample> gpuSamples;
};

namespace
{
constexpr bool ModelOptimizationConfig::* ModelPolicies[]={&ModelOptimizationConfig::reuseFrameBuffers,&ModelOptimizationConfig::skipUnchangedUploads,&ModelOptimizationConfig::cacheStaticMeshes,&ModelOptimizationConfig::cacheInfluences,&ModelOptimizationConfig::cacheSkinTransforms};
bool SameOptimizations(const ModelOptimizationConfig& a,const ModelOptimizationConfig& b) noexcept
{for(const auto flag:ModelPolicies)if(a.*flag!=b.*flag)return false;return true;}
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
    float px, py, pz, nx, ny, nz, u, v, tx, ty, tz, tw, cr, cg, cb, ca;
};

ModelVertex PackVertex(const Vertex& vertex)
{
    return {vertex.position.x, vertex.position.y, vertex.position.z,
        vertex.normal.x, vertex.normal.y, vertex.normal.z, vertex.uv.x, vertex.uv.y,
        vertex.tangent.x, vertex.tangent.y, vertex.tangent.z, vertex.tangent.w,
        vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w};
}

std::vector<ModelVertex> BuildVertices(const MeshData& mesh)
{
    std::vector<ModelVertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for(const auto& vertex : mesh.vertices)
        vertices.push_back(PackVertex(vertex));
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
const char* ComputeSkinningUnavailable(const RHI::IDevice& device,const RHI::ShaderDesc& shader) noexcept
{
    try {
        if(!device.Supports(RHI::Feature::Compute))return "Compute skinning is not supported by this device";
        if(shader.stage!=RHI::ShaderStage::Compute||!shader.binary||!shader.binarySize||!shader.entryPoint||!*shader.entryPoint)
            return "Model compute shader unavailable for this backend";
        return "";
    } catch(...) {return "Model compute availability check failed";}
}
}

ModelRenderer::ModelRenderer(Renderer& renderer) : m_renderer(renderer), m_profiling(std::make_unique<ProfilingState>()) {InitializeLiveProfiling();}

void ModelRenderer::InitializeLiveProfiling() noexcept
{
    if(!Platform::IsLiveCollectorEnabled()||m_profiling->skinningControl.GetId())return;
    try {
        m_renderer.InitializeLiveProfiling();
        const auto device=Platform::FindLiveObject(&m_renderer.GetDevice());
        if(!m_profiling->id){m_profiling->id=Platform::RegisterLiveObject(this,{Platform::LiveObjectKind::Model,device,m_renderer.GetLiveSourceId(),0,0,"ModelRenderer"});m_liveAppliedMode=m_mode;}
        Platform::ControlDesc desc;desc.device=device;desc.source=m_renderer.GetLiveSourceId();desc.owner=m_profiling->id;
        desc.key="model.skinning";desc.label="Skinning execution";desc.kind=Platform::ControlKind::Enumeration;
        desc.application=Platform::ControlApplication::ResourceRebuild;desc.appliedValue=std::int64_t(m_liveAppliedMode);
        desc.choices={{0,"Vertex shader"},{1,"Compute pre-skin"}};
        desc.category=Platform::ControlCategory::Optimization;desc.description="Select Model deformation execution stage";desc.onPath="Compute pre-skin dispatch writes a vertex buffer before raster draws";desc.offPath="Vertex shader deformation in each raster/shadow pass";desc.conditions="Compute requires the device feature and an actual Model compute shader for this backend";desc.tradeoffs="Compute dispatch, output buffers and barriers versus repeated vertex shader work; CPU/GPU scopes can overlap";
        const auto reason=ComputeSkinningUnavailable(m_renderer.GetDevice(),ShaderDescription(2,GetModelComputeShader()));desc.writable=!*reason;desc.unavailableReason=reason;
        m_profiling->skinningControl=Platform::RegisterControl(std::move(desc),[this](const Platform::ControlRequest& request,Platform::LiveFrameKey){
            const auto mode=static_cast<SkinningExecutionMode>(std::get<std::int64_t>(request.value));
            if(mode!=m_mode&&mode==SkinningExecutionMode::ComputePreSkin&&!PrepareComputePipeline())
                return Platform::ControlApplyResult{Platform::ControlState::Failed,std::int64_t(m_mode),"Compute skinning pipeline could not be prepared"};
            const bool applied=SetSkinningExecutionMode(mode);
            return Platform::ControlApplyResult{applied?Platform::ControlState::Applied:Platform::ControlState::Failed,std::int64_t(m_mode),applied?"":"Compute skinning pipeline could not be prepared"};
        });
        const char* keys[]={"model.frameBufferReuse","model.skipUnchangedUploads","model.staticMeshCache","model.influenceCache","model.skinTransformCache"};
        const char* descriptions[]={"Reuse completed Model frame resources","Skip equal Model upload bytes","Reuse immutable compute input meshes","Reuse packed and validated skin influences","Share identical skin influence transforms"};
        const char* onPaths[]={"Retain size/stride/usage-compatible buffers and per-frame sets","Compare cached bytes before recording uploads","Keep each immutable source mesh's compute input buffer","Compare influence bytes/counts before reusing packed influences","Blend one transform per byte-identical influence group and palette; evaluate every exact bounds position"};
        const char* offPaths[]={"Wait for a completed frame slot and replace its buffers/sets","Upload every used palette/morph input each frame","Recreate each used compute input mesh once per frame","Pack, validate and upload the complete influences each frame","Blend the skin transform separately for every bounds vertex"};
        for(std::size_t i=0;i<m_profiling->optimizationControls.size();++i){Platform::ControlDesc d;d.device=device;d.source=m_renderer.GetLiveSourceId();d.owner=m_profiling->id;d.key=keys[i];d.label=descriptions[i];d.description=descriptions[i];d.category=Platform::ControlCategory::Optimization;d.appliedValue=m_optimizationConfig.*ModelPolicies[i];d.onPath=onPaths[i];d.offPath=offPaths[i];d.conditions=i==2?"Model ComputePreSkin path; immutable source meshes only, morphed inputs remain dynamic":"Model preparation after the frame boundary; per-entity palette checks and completion waits remain enabled";d.tradeoffs="CPU comparisons and retained buffers versus upload/allocation/command work; failed submissions still roll back";
            if(i==2&&m_mode!=SkinningExecutionMode::ComputePreSkin){d.writable=false;d.unavailableReason="Static mesh input cache is used only by ComputePreSkin";}
            if(i==4){d.conditions="Advanced HDR automatic compute skinning; identical influence bytes and current per-entity palette only";d.tradeoffs="Retain influence group indices and temporary transforms to avoid repeated matrix blending; exact vertex bounds, morph positions and deformation history stay unchanged";}
            m_profiling->optimizationControls[i]=Platform::RegisterControl(std::move(d),[this,i](const Platform::ControlRequest& request,Platform::LiveFrameKey){auto next=GetOptimizationConfig();next.*ModelPolicies[i]=std::get<bool>(request.value);SetOptimizationConfig(next);m_profiling->optimizationRequests[i]=request.request;return Platform::ControlApplyResult{Platform::ControlState::PendingApply,m_optimizationConfig.*ModelPolicies[i],{}};});
        }
        const char* catalogKeys[]={"model.frameDescriptorCache","model.lazyUploadCommands","model.sharedAnimationPalettes","model.zeroMorphSkip"};
        const char* descriptionsReadOnly[]={"Cache per-frame Model compute resource sets","Acquire Model upload commands only when needed","Share eligible animation palettes","Skip zero-weight morph targets"};
        const char* conditionsReadOnly[]={"Completed frame resources and matching compute bindings","Model preparation with no uploads or dispatches can return without an upload command list","ModelScene animation/skin evaluation; updates outside Render have no current source-frame attribution","ModelScene morph evaluation; updates outside Render have no current source-frame attribution"};
        for(std::size_t i=0;i<m_profiling->catalog.size();++i){Platform::ControlDesc d;d.device=device;d.source=m_renderer.GetLiveSourceId();d.owner=m_profiling->id;d.key=catalogKeys[i];d.label=descriptionsReadOnly[i];d.description=descriptionsReadOnly[i];d.category=Platform::ControlCategory::Optimization;d.appliedValue=true;d.writable=false;d.onPath=conditionsReadOnly[i];d.offPath="No equivalent runtime reference path is implemented";d.conditions=conditionsReadOnly[i];d.tradeoffs=i<2?"Observation only; a separate equivalent experiment is needed":"Animation evaluation may occur outside the render frame; separate observation/experiment is needed";d.unavailableReason=d.offPath;m_profiling->catalog[i]=Platform::RegisterControl(std::move(d),{});}
    } catch(...) {}
}

void ModelRenderer::PublishLiveAvailability() noexcept
{
    if(!m_profiling->skinningControl.GetId())return;
    const auto reason=ComputeSkinningUnavailable(m_renderer.GetDevice(),ShaderDescription(2,GetModelComputeShader()));
    Platform::PublishControlAvailability(m_profiling->skinningControl.GetId(),!*reason,reason);
    const bool compute=m_mode==SkinningExecutionMode::ComputePreSkin;Platform::PublishControlAvailability(m_profiling->optimizationControls[2].GetId(),compute,compute?"":"Static mesh input cache is used only by ComputePreSkin");
}

void ModelRenderer::PrepareLiveFrame(Platform::LiveFrameKey frame) noexcept
{
    if(m_pendingOptimizationConfig){m_optimizationConfig=*m_pendingOptimizationConfig;m_pendingOptimizationConfig.reset();}
    if(!Platform::IsLiveCollectorEnabled())return;
    PublishLiveAvailability();
    Platform::NotifyLiveSettingsApplied(m_renderer.GetLiveSourceId(),frame,m_liveAppliedMode!=m_mode||!SameOptimizations(m_liveAppliedOptimizationConfig,m_optimizationConfig));
    m_liveAppliedMode=m_mode;
    m_liveAppliedOptimizationConfig=m_optimizationConfig;
    Platform::PublishControlValue(m_profiling->skinningControl.GetId(),std::int64_t(m_mode));
    for(std::size_t i=0;i<m_profiling->optimizationControls.size();++i)Platform::PublishControlValue(m_profiling->optimizationControls[i].GetId(),m_optimizationConfig.*ModelPolicies[i]);
    Platform::PollControls(m_profiling->id,frame);
    PublishLiveAvailability();
    if(m_pendingOptimizationConfig){m_optimizationConfig=*m_pendingOptimizationConfig;m_pendingOptimizationConfig.reset();}
    Platform::NotifyLiveSettingsApplied(m_renderer.GetLiveSourceId(),frame,!SameOptimizations(m_liveAppliedOptimizationConfig,m_optimizationConfig));
    for(std::size_t i=0;i<m_profiling->optimizationControls.size();++i){const bool value=m_optimizationConfig.*ModelPolicies[i];if(m_profiling->optimizationRequests[i]){Platform::CompleteControl(m_profiling->optimizationRequests[i],frame,value);m_profiling->optimizationRequests[i]=0;}Platform::PublishControlValue(m_profiling->optimizationControls[i].GetId(),value);}
    m_liveAppliedOptimizationConfig=m_optimizationConfig;
    m_liveAppliedMode=m_mode;
    Platform::PublishControlValue(m_profiling->skinningControl.GetId(),std::int64_t(m_mode));
}

ModelRenderer::~ModelRenderer()
{
    for(auto& control:m_profiling->optimizationControls)control=Platform::ControlRegistration{};
    for(auto& control:m_profiling->catalog)control=Platform::ControlRegistration{};
    m_profiling->skinningControl=Platform::ControlRegistration{};
    Platform::RetireLiveObject(m_profiling->id);
    (void)RestoreShaders();
    auto& device = m_renderer.GetDevice();
    ReleaseFrameBuffers();
    ReleaseMeshBuffers();
    for(const auto& sample : m_profiling->gpuSamples) device.DestroyTimestampQuery(sample.query);
    if(m_computePipeline) device.DestroyPipeline(m_computePipeline);
    if(m_computeShader) device.DestroyShader(m_computeShader);
}

bool ModelRenderer::SetSkinningExecutionMode(SkinningExecutionMode mode)
{
    if(mode > SkinningExecutionMode::ComputePreSkin) return Failure("Unknown skinning execution mode.");
    if(mode == SkinningExecutionMode::ComputePreSkin && !m_renderer.GetDevice().Supports(RHI::Feature::Compute))
        return Failure("Compute skinning is not supported by this device.");
    if(m_mode != mode) InvalidateHistory();
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
        InvalidateHistory();
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
    m_originalStockShadowCoverage = shaders.stockShadowCoverage;
    m_configured = true;
    const auto vertex = ShaderDescription(0, GetModelVertexShader(m_renderer.GetLighting().enabled && m_renderer.GetLighting().shadows));
    const auto shadow = ShaderDescription(1, GetModelShadowShader());
    // Only the built-in Model shadow shader guarantees stock UV/alpha varyings.
    const bool stockShadowCoverage=m_shaderBytes[1].empty();
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
        shaders.stockShadowCoverage==stockShadowCoverage &&
        SameShader(shaders.meshVertex, vertex) && SameShader(shaders.shadowVertex, shadow)) return true;

    // 모델 단계만 갱신한다. 사용자가 선택한 Fragment, Canvas, ToneMap 단계는 유지한다.
    shaders.meshVertex = vertex;
    shaders.shadowVertex = shadow;
    shaders.vertexBindings = bindings;
    shaders.additionalConstantBytes = 16;
    shaders.stockShadowCoverage = stockShadowCoverage;
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
        shaders.stockShadowCoverage = m_originalStockShadowCoverage;
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
        {bindings.data(), static_cast<uint32_t>(bindings.size()), 16, RHI::ShaderStageFlags::Compute, 10}, {64, 1, 1}});
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
    InvalidateHistory();
    m_draws.clear();
    for(auto& frame : m_frames) ReleaseFrame(frame);
    m_frames.clear();
    if(m_influenceBuffer) m_renderer.GetDevice().DestroyBuffer(m_influenceBuffer);
    m_influenceBuffer = nullptr;
    m_influences.clear();
    m_influenceOffsets.clear();
    m_influenceCounts.clear(); m_requiredInfluenceJoints.clear(); m_validInfluences.clear();
    m_skinTransformGroups.clear();
}

void ModelRenderer::InvalidateHistory()
{
    m_historyScene = nullptr;
    m_historyFrame = UINT32_MAX;
    m_historyDraws.clear();
    m_historyMeshes.clear();
    m_historyVertexCounts.clear();
}

void ModelRenderer::ReleaseFrame(FrameResources& frame)
{
    auto& device = m_renderer.GetDevice();
    for(auto& set : frame.sets) if(set.handle) device.DestroyResourceSet(set.handle);
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
    while(!m_profiling->gpuSamples.empty() && device.IsComplete(m_profiling->gpuSamples.front().completion))
    {
        const auto sample = m_profiling->gpuSamples.front();
        uint64_t ticks[2];
        const bool read=device.ReadTimestamps(sample.query,0,2,ticks);
        if(read)
        {
            const uint32_t bits = device.GetTimestampValidBits();
            const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
            m_gpuMilliseconds = ((ticks[1] - ticks[0]) & mask) * device.GetTimestampPeriodNanoseconds() / 1e6;
        }
        if(sample.liveSpan.id){auto span=sample.liveSpan;span.outcome=Platform::LiveOutcome::Completed;span.duration.state=Platform::LiveValueState::Unavailable;
            const auto bits=device.GetTimestampValidBits();const auto period=device.GetTimestampPeriodNanoseconds();if(read&&bits&&bits<=64&&period>0){const auto mask=bits==64?UINT64_MAX:(std::uint64_t(1)<<bits)-1;span.duration={Platform::LiveValueState::Ready,double((ticks[1]-ticks[0])&mask)*period/1e6};if(sample.calibration)Platform::Detail::CalibrateGpuSpan(span,*sample.calibration,ticks[0],ticks[1],Platform::FindLiveObject(&device),sample.submittedNs,Platform::LiveNowNs());}Platform::RecordLiveGpuSpan(span);
        }
        device.DestroyTimestampQuery(sample.query);
        m_profiling->gpuSamples.pop_front();
    }
}

bool ModelRenderer::Prepare(const ModelScene& scene)
{
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
    Platform::CpuScope prepareTiming(Platform::LiveStage::CpuPrepare,"Model preparation");
#endif
    static_assert(sizeof(ModelVertex) == 64 && offsetof(ModelVertex, cr) == 48);
    static_assert(sizeof(SkinInfluence) == 48 && offsetof(SkinInfluence, dqBlendWeight) == 32);
    static_assert(sizeof(SkinJointMatrices) == 176);
    auto& device = m_renderer.GetDevice();
    const auto& config = m_renderer.GetConfig();
    const bool stockGeometry = config.advanced.enabled && config.enableHdrRendering;
    bool hasSkin = false;
    for(uint32_t index = 0; index < scene.GetEntityCount(); ++index)
        hasSkin |= scene.GetEntitySkinPaletteOffset(static_cast<EntityID>(index)) != UINT32_MAX;
    const bool compute = stockGeometry ? hasSkin && device.Supports(RHI::Feature::Compute) :
        m_mode == SkinningExecutionMode::ComputePreSkin;
    if(compute && !PrepareComputePipeline()) return false;
    if(!stockGeometry || m_historyScene != &scene) InvalidateHistory();

    // Compare actual influence bytes, including edits made without changing mesh identity.
    // Packing and immutable validation are needed only when those bytes/counts change.
    bool sameInfluences = m_optimizationConfig.cacheInfluences && m_influenceBuffer && m_influenceOffsets.size() == scene.Meshes().size();
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
                double totalWeight = 0.0;
                if(!std::isfinite(influence.dqBlendWeight) || influence.dqBlendWeight < 0 || influence.dqBlendWeight > 1)
                    validInfluences[index] = false;
                for(uint32_t component = 0; component < 4; ++component)
                {
                    const float weight = influence.weights[component];
                    totalWeight += weight;
                    if(!std::isfinite(weight) || weight < 0)
                        validInfluences[index] = false;
                    if(weight > 0)
                    {
                        const uint32_t joint = influence.jointIndices[component];
                        requiredJoints[index] = std::max(requiredJoints[index], uint64_t(joint) + 1);
                    }
                }
                if(!std::isfinite(totalWeight) || totalWeight > std::numeric_limits<float>::max())
                    validInfluences[index] = false;
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

    const uint32_t inFlight = std::max(1u, device.GetDesc().maxFramesInFlight);
    if(stockGeometry && inFlight == UINT32_MAX)
        return Failure("Model frame history exceeds the frame slot limit.");
    // Surface motion reads the previous deformation as well as this frame's
    // output. Keep its slot in addition to the configured in-flight outputs.
    const uint32_t limit = inFlight + (stockGeometry ? 1u : 0u);
    uint32_t selected = 0;
    for(; selected < m_frames.size(); ++selected)
        if(selected != m_historyFrame && (!m_frames[selected].completion || device.IsComplete(m_frames[selected].completion))) break;
    if(selected == m_frames.size())
    {
        if(m_frames.size() < limit) m_frames.emplace_back();
        else
        {
            selected = (m_frameIndex + 1) % static_cast<uint32_t>(m_frames.size());
            if(selected == m_historyFrame) selected = (selected + 1) % static_cast<uint32_t>(m_frames.size());
            if(!device.Wait(m_frames[selected].completion, UINT64_MAX))
                return Failure("Model frame completion wait failed.");
        }
    }
    m_frameIndex = selected;
    auto& frame = m_frames[selected];
    if(!m_optimizationConfig.reuseFrameBuffers)ReleaseFrame(frame);
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
            m_skinTransformGroups.clear();
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
        const bool changed = m_influences.size() != influences.size() ||
            (!influences.empty() && std::memcmp(m_influences.data(), influences.data(), influences.size() * sizeof(SkinInfluence)) != 0);
        auto* buffer = createUpload(influences.data(), influences.size(), sizeof(SkinInfluence),
            RHI::BufferUsage::Storage, RHI::ResourceState::ShaderResource);
        if(!buffer) return Failure("Skin influence upload failed.");
        if(m_influenceBuffer) device.DestroyBuffer(m_influenceBuffer);
        m_influenceBuffer = buffer;
        m_influences = std::move(influences); m_influenceOffsets = std::move(offsets);
        m_influenceCounts = std::move(counts); m_requiredInfluenceJoints = std::move(requiredJoints);
        m_validInfluences = std::move(validInfluences);
        if(changed && !++m_influenceRevision) ++m_influenceRevision;
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
        if(index != selected && index != m_historyFrame && m_frames[index].workingSet != workingSet &&
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
        if(m_optimizationConfig.skipUnchangedUploads && cached->bytes.size() == bytes && std::memcmp(cached->bytes.data(), data, bytes) == 0)
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
        if(m_optimizationConfig.skipUnchangedUploads)cached->bytes.assign(first, first + bytes);else cached->bytes.clear();cached->state = state;
        return cached->buffer;
    };
    const SkinJointMatrices identity;
    auto* paletteBuffer = upload(palette.empty() ? &identity : palette.data(), palette.empty() ? 1 : palette.size(),
        sizeof(SkinJointMatrices), RHI::BufferUsage::Storage, RHI::ResourceState::ShaderResource);
    if(!paletteBuffer) return Failure("Skin palette upload failed.");
    if(compute&&!m_optimizationConfig.cacheStaticMeshes)ReleaseMeshBuffers();
    if(m_meshBuffers.size() != scene.Meshes().size()) { ReleaseMeshBuffers(); m_meshBuffers.resize(scene.Meshes().size()); }
    std::vector<bool> meshUsed(m_meshBuffers.size(), false);
    m_geometrySignatures.resize(scene.GetEntityCount());
    m_geometryRevisions.resize(scene.GetEntityCount());
    if(m_optimizationConfig.cacheSkinTransforms) m_skinTransformGroups.resize(scene.Meshes().size());
    std::vector<Detail::SkinTransform> skinTransforms;
    for(uint32_t index = 0; prepared && index < scene.GetEntityCount(); ++index)
    {
        const auto entity = static_cast<EntityID>(index);
        auto& draw = m_draws[index];
        const uint32_t paletteOffset = scene.GetEntitySkinPaletteOffset(entity);
        const auto mesh = scene.GetEntityMesh(entity);
        const uint32_t meshIndex = ToIndex(mesh);
        const auto* morphed = scene.TryGetEntityMorphedMesh(entity);
        const bool deformed = paletteOffset != UINT32_MAX || morphed || scene.HasEntityMorphTargets(entity);
        draw.stockGeometry = stockGeometry;
        if(stockGeometry && deformed)
        {
            if(!IsValid(mesh) || meshIndex >= scene.Meshes().size())
                return Failure("A deformed entity requires valid source geometry.");
            const auto& sourceMesh = scene.Meshes()[meshIndex];
            const auto& input = morphed ? *morphed : *sourceMesh;
            if(input.vertices.empty() || input.vertices.size() != sourceMesh->vertices.size())
                return Failure("Deformation must preserve source vertex indexing.");
            std::vector<SkinJointMatrices> entityPalette;
            const auto& influences = scene.GetMeshSkinInfluences(mesh);
            if(paletteOffset != UINT32_MAX)
            {
                const size_t count = static_cast<size_t>(m_requiredInfluenceJoints[meshIndex]);
                if(count) entityPalette.assign(palette.begin() + paletteOffset, palette.begin() + paletteOffset + count);
            }
            const bool cpuVerticesNeeded = !compute || paletteOffset == UINT32_MAX;
            std::vector<ModelVertex> cpuVertices;
            if(cpuVerticesNeeded) cpuVertices.reserve(input.vertices.size());
            uint64_t signature = 1469598103934665603ull;
            const auto hashBytes = [&signature](const void* data, size_t size) {
                const auto* bytes = static_cast<const uint8_t*>(data);
                for(size_t byte = 0; byte < size; ++byte)
                { signature ^= bytes[byte]; signature *= 1099511628211ull; }
            };
            const auto identity = reinterpret_cast<uintptr_t>(sourceMesh.get());
            hashBytes(&identity, sizeof(identity));
            // Compute already evaluates normals/tangents. On the CPU evaluate
            // only the position needed for exact bounds, and identify geometry
            // by its complete deterministic inputs rather than re-skinning it.
            const bool positionsOnly = compute && paletteOffset != UINT32_MAX;
            if(positionsOnly)
            {
                const size_t vertexCount = input.vertices.size();
                hashBytes(&vertexCount, sizeof(vertexCount));
                hashBytes(&m_influenceRevision, sizeof(m_influenceRevision));
                hashBytes(&m_influenceOffsets[meshIndex], sizeof(m_influenceOffsets[meshIndex]));
                hashBytes(entityPalette.data(), entityPalette.size() * sizeof(SkinJointMatrices));
            }
            const SkinTransformGroups* transformGroups = nullptr;
            if(positionsOnly && m_optimizationConfig.cacheSkinTransforms)
            {
                auto& groups = m_skinTransformGroups[meshIndex];
                // Group indices depend only on the published influence bytes and
                // their mesh range, never on source/morph positions or a palette.
                if(groups.influenceRevision != m_influenceRevision ||
                    groups.influenceOffset != m_influenceOffsets[meshIndex] ||
                    groups.influenceCount != influences.size())
                {
                    if(!Detail::BuildSkinTransformGroups(influences, groups.representatives, groups.vertexGroups))
                        return Failure("Skin transform groups exceed the supported index range.");
                    groups.influenceRevision = m_influenceRevision;
                    groups.influenceOffset = m_influenceOffsets[meshIndex];
                    groups.influenceCount = static_cast<uint32_t>(influences.size());
                }
                // No benefit when every vertex has a different influence.
                if(groups.representatives.size() < influences.size())
                {
                    skinTransforms.resize(groups.representatives.size());
                    for(size_t group = 0; group < groups.representatives.size(); ++group)
                        if(!Detail::BuildSkinTransform(influences[groups.representatives[group]], entityPalette, skinTransforms[group]))
                            return Failure("Deformed bounds evaluation failed.");
                    transformGroups = &groups;
                }
            }
            for(size_t vertexIndex = 0; vertexIndex < input.vertices.size(); ++vertexIndex)
            {
                Vertex output = input.vertices[vertexIndex];
                if(paletteOffset != UINT32_MAX)
                {
                    bool valid = true;
                    if(transformGroups)
                        output.position = Detail::ApplySkinTransform(skinTransforms[transformGroups->vertexGroups[vertexIndex]], output.position);
                    else
                        valid = positionsOnly ?
                            Detail::SkinPosition(input.vertices[vertexIndex], influences[vertexIndex], entityPalette, output.position) :
                            SkinVertex(input.vertices[vertexIndex], influences[vertexIndex], entityPalette, output);
                    if(!valid) return Failure("Deformed bounds evaluation failed.");
                }
                if(!std::isfinite(output.position.x) || !std::isfinite(output.position.y) || !std::isfinite(output.position.z))
                    return Failure("Deformed geometry contains a non-finite position.");
                draw.localBounds.Include(output.position);
                // Scene meshes are immutable after registration. Their owner
                // identity plus skin inputs determines compute output; only
                // a per-entity morph input needs its vertex bytes inspected.
                if(!positionsOnly || morphed)
                {
                    const auto packed = PackVertex(positionsOnly ? input.vertices[vertexIndex] : output);
                    hashBytes(&packed, sizeof(packed));
                    if(cpuVerticesNeeded) cpuVertices.push_back(packed);
                }
            }
            // CPU·GPU 연산 순서 차이로 경계 정점이 컬링되지 않도록 작은 여유를 둔다.
            const auto& bounds = draw.localBounds;
            const float magnitude = std::max({1.f, std::fabs(bounds.min.x), std::fabs(bounds.min.y),
                std::fabs(bounds.min.z), std::fabs(bounds.max.x), std::fabs(bounds.max.y), std::fabs(bounds.max.z)});
            const auto padding = Math::float3(magnitude * 1e-4f, magnitude * 1e-4f, magnitude * 1e-4f);
            draw.localBounds.min = draw.localBounds.min - padding;
            draw.localBounds.max = draw.localBounds.max + padding;
            draw.hasLocalBounds = true;
            const bool changedOwner = positionsOnly && (m_historyScene != &scene ||
                index >= m_historyMeshes.size() || m_historyMeshes[index].lock() != sourceMesh);
            if(!m_geometryRevisions[index] || m_geometrySignatures[index] != signature || changedOwner)
            {
                m_geometrySignatures[index] = signature;
                m_geometryRevisions[index] = m_nextGeometryRevision++;
                if(!m_nextGeometryRevision) ++m_nextGeometryRevision;
            }
            draw.geometryRevision = m_geometryRevisions[index];
            draw.deformationHistoryValid = false;
            if(m_historyScene == &scene && index < m_historyDraws.size() && index < m_historyMeshes.size() &&
                index < m_historyVertexCounts.size() && m_historyMeshes[index].lock() == sourceMesh &&
                m_historyVertexCounts[index] == input.vertices.size() && m_historyDraws[index].vertexBuffer)
            {
                draw.previousVertexBuffer = m_historyDraws[index].vertexBuffer;
                draw.deformationHistoryValid = true;
            }
            if(cpuVerticesNeeded)
                draw.vertexBuffer = upload(cpuVertices.data(), cpuVertices.size(), sizeof(ModelVertex),
                    RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage, RHI::ResourceState::VertexBuffer);
        }
        if(morphed && !draw.vertexBuffer)
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
        if(!stockGeometry)
        {
            draw.vertexResources = {
                {11, 0, influenceBuffer, nullptr, 0, influenceBuffer->GetDesc().size, {}},
                {12, 0, paletteBuffer, nullptr, 0, paletteBuffer->GetDesc().size, {}}};
            const std::array<uint32_t, 4> constants = {
                compute || !IsValid(mesh) || meshIndex >= m_influenceOffsets.size() ? UINT32_MAX : m_influenceOffsets[meshIndex], paletteOffset, 0, 0};
            draw.inlineConstants.resize(sizeof(constants));
            std::memcpy(draw.inlineConstants.data(), constants.data(), sizeof(constants));
        }
    }
    Platform::LiveGpuSpan liveCompute;
    RHI::TimestampQueryHandle query = nullptr;
    ScopeExit discardQuery{[&] { if(query) device.DestroyTimestampQuery(query); }};
    if(frame.sets.size() < scene.GetEntityCount()) frame.sets.resize(scene.GetEntityCount());
    for(size_t index = 0; index < m_meshBuffers.size(); ++index)
        if(!meshUsed[index])
        {
            if(m_meshBuffers[index].buffer) device.DestroyBuffer(m_meshBuffers[index].buffer);
            m_meshBuffers[index] = {};
        }
    if(compute && prepared && acquire())
    {
        if(Platform::IsLiveCollectorEnabled()){const auto context=Platform::GetLiveContext();liveCompute.id=Platform::AllocateLiveId();liveCompute.frame=context.frame;liveCompute.appliedRevision=context.appliedRevision;liveCompute.owner=m_profiling->id;liveCompute.scope=Platform::LiveGpuScope::ModelCompute;liveCompute.duration.state=device.Supports(RHI::Feature::TimestampQuery)?Platform::LiveValueState::Pending:Platform::LiveValueState::Unsupported;}
        if(device.Supports(RHI::Feature::TimestampQuery)) query = device.CreateTimestampQuery({2});
        if(query) { commands->ResetTimestamps(query, 0, 2); commands->WriteTimestamp(query, 0); }
        if(liveCompute.id)liveCompute.begin.command=commands->GetLiveCommandPosition();
        commands->BindComputePipeline(m_computePipeline);
        for(uint32_t index = 0; index < scene.GetEntityCount(); ++index)
        {
            const auto entity = static_cast<EntityID>(index);
            const uint32_t paletteOffset = scene.GetEntitySkinPaletteOffset(entity);
            if(paletteOffset == UINT32_MAX)
            {
                if(frame.sets[index].handle) device.DestroyResourceSet(frame.sets[index].handle);
                frame.sets[index] = {};
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
            bool matching = set.handle && set.pipeline == m_computePipeline;
            for(uint32_t binding = 0; matching && binding < bindings.size(); ++binding)
                matching = set.bindings[binding].buffer == bindings[binding].buffer &&
                    set.bindings[binding].size == bindings[binding].size;
            if(!matching)
            {
                if(set.handle) device.DestroyResourceSet(set.handle);
                set = {};
                set.handle = device.CreateResourceSet({m_computePipeline, bindings.data(), static_cast<uint32_t>(bindings.size())});
                if(!set.handle) { prepared = false; break; }
                set.pipeline = m_computePipeline;
                set.bindings = bindings;
            }
            const RHI::ResourceBarrierDesc before{source, nullptr, RHI::ResourceState::VertexBuffer, RHI::ResourceState::ShaderResource, {}};
            commands->ResourceBarrier(&before, 1);
            if(cached->state != RHI::ResourceState::UnorderedAccess)
            {
                const RHI::ResourceBarrierDesc ready{output, nullptr, cached->state, RHI::ResourceState::UnorderedAccess, {}};
                commands->ResourceBarrier(&ready, 1);
            }
            commands->BindResourceSet(set.handle);
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
        if(liveCompute.id)liveCompute.end.command=commands->GetLiveCommandPosition();
    }
    // Shrink completed-slot storage to this scene's working set (no historical entity cache).
    for(size_t index = cursor; index < frame.buffers.size(); ++index) device.DestroyBuffer(frame.buffers[index].buffer);
    frame.buffers.resize(cursor);
    for(size_t index = scene.GetEntityCount(); index < frame.sets.size(); ++index)
        if(frame.sets[index].handle) device.DestroyResourceSet(frame.sets[index].handle);
    frame.sets.resize(scene.GetEntityCount());
    if(!compute) for(auto& set : frame.sets) { if(set.handle) device.DestroyResourceSet(set.handle); set = {}; }
    if(!commands) { submitted = prepared; return prepared; }
    const bool closed = commands->Close();
    RHI::FenceHandle completion;
    submitted = prepared && closed && device.Submit({&commands, 1, nullptr, 0}, completion);
    // Publish ownership before an allocation in the timestamp handoff can throw.
    if(submitted) frame.completion = completion;
    if(liveCompute.id){liveCompute.begin.submission=liveCompute.end.submission=device.GetLiveSubmissionId(completion);liveCompute.outcome=submitted?Platform::LiveOutcome::Submitted:Platform::LiveOutcome::Failed;if(!submitted)liveCompute.duration.state=Platform::LiveValueState::Unavailable;Platform::RecordLiveGpuSpan(liveCompute);}
    if(query && submitted) { std::optional<RHI::TimestampCalibration> calibration;if(liveCompute.id){RHI::TimestampCalibration pair;if(device.GetTimestampCalibration(pair))calibration=pair;}m_profiling->gpuSamples.push_back({query, completion,liveCompute,calibration,Platform::LiveNowNs()}); query = nullptr; }
    if(!submitted) return Failure("Model deformation upload or compute submission failed.");
    return true;
}

bool ModelRenderer::RenderScene(const ModelScene& scene, const Camera* camera, const Canvas* overlay, Image* readback)
{
    InitializeLiveProfiling();
    m_renderer.PublishLiveGeometryAvailability(true);
    Platform::LiveFrameScope liveFrame(Platform::FindLiveObject(&m_renderer.GetDevice()),m_renderer.GetLiveSourceId());
    m_renderer.GetDevice().PrepareLiveControls(liveFrame.GetKey());
    bool rendered = false;
    bool drawAttempted = false;
    RHI::FenceHandle previousDraw;
    uint32_t consumedHistoryFrame = UINT32_MAX;
    ScopeExit retainDrawCompletion{[&] {
        if(!drawAttempted) return;
        const auto completion = m_renderer.GetLastRenderCompletion();
        if(completion && completion != previousDraw)
        {
            m_frames[m_frameIndex].completion = completion;
            // 이전 변형 버퍼도 이번 Surface 패스가 읽으므로 같은 완료 시점까지 보존한다.
            if(consumedHistoryFrame < m_frames.size()) m_frames[consumedHistoryFrame].completion = completion;
        }
    }};
    try
    {
        if(readback) *readback = {};
        CollectGpuSamples();
        PrepareLiveFrame(liveFrame.GetKey());
        const auto& config = m_renderer.GetConfig();
        const bool stockGeometry = config.advanced.enabled && config.enableHdrRendering;
        if(stockGeometry && std::any_of(m_shaderBytes.begin(), m_shaderBytes.end(),
            [](const auto& bytes) { return !bytes.empty(); }))
        {
            InvalidateHistory();
            return Failure("Advanced model rendering requires the standard deformation shaders.");
        }
        // 이전 복원 자체가 실패했다면 원래 설정을 덮어쓰지 않고 다시 복원부터 처리한다.
        if(RestoreShaders() && (stockGeometry || ConfigureShaders()) &&
            m_renderer.PrepareLiveFrame(liveFrame.GetKey()) && Prepare(scene))
        {
            previousDraw = m_renderer.GetLastRenderCompletion();
            consumedHistoryFrame = m_historyFrame;
            drawAttempted = true;
            rendered = camera ? m_renderer.Render(scene, *camera, m_draws, overlay, readback) :
                m_renderer.Render(scene, m_draws, overlay, readback);
            const auto completion = m_renderer.GetLastRenderCompletion();
            if(rendered && stockGeometry && completion && completion != previousDraw)
            {
                // 확보 실패로 생략한 프레임은 이전 변형 기준을 전진시키지 않는다.
                m_historyDraws = m_draws;
                m_historyMeshes.resize(scene.GetEntityCount());
                m_historyVertexCounts.resize(scene.GetEntityCount());
                for(uint32_t index = 0; index < scene.GetEntityCount(); ++index)
                {
                    const auto mesh = scene.GetEntityMesh(static_cast<EntityID>(index));
                    if(IsValid(mesh) && ToIndex(mesh) < scene.Meshes().size())
                    {
                        m_historyMeshes[index] = scene.Meshes()[ToIndex(mesh)];
                        m_historyVertexCounts[index] = scene.Meshes()[ToIndex(mesh)]->vertices.size();
                    }
                    else { m_historyMeshes[index].reset(); m_historyVertexCounts[index] = 0; }
                }
                m_historyScene = &scene;
                m_historyFrame = m_frameIndex;
            }
        }
    }
    catch(const std::exception& error) { (void)Failure(error.what()); }
    catch(...) { (void)Failure("Model rendering failed."); }
    // 성공, 준비 실패, 예외 경로 모두 복원을 예약한 후 호출자에게 돌아간다.
    const bool restored = RestoreShaders();
    liveFrame.SetOutcome(rendered&&restored?Platform::LiveOutcome::Completed:Platform::LiveOutcome::Failed);
    if(!rendered || !restored) InvalidateHistory();
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
