#include "../Platform/Profiler/RhiProfiler.h"
#include "RendererInternal.h"
#include "dyf/RenderExtension.h"
#include <utility>

namespace dyf
{
Renderer::Renderer(RHI::IDevice& device, const void* window, const RendererConfig& config)
    : m_impl(std::make_unique<Impl>(*this, device, window, config))
{
}

Renderer::~Renderer() = default;
const RendererConfig& Renderer::GetAppliedConfig() const { return m_impl->config; }
Platform::LiveId Renderer::GetLiveSourceId() const noexcept { return m_impl->liveId; }
void Renderer::InitializeLiveProfiling() noexcept { m_impl->InitializeLiveProfiling(); }
bool Renderer::PrepareLiveFrame(Platform::LiveFrameKey frame) noexcept { return m_impl->PrepareLiveFrame(frame); }
void Renderer::PublishLiveGeometryAvailability(bool custom) noexcept { m_impl->PublishLiveGeometryAvailability(custom); }

bool Renderer::Render(const MeshData& mesh, Image* readback)
{
    return m_impl->Render(mesh, readback);
}

bool Renderer::Render(const MeshData& mesh, const Camera& camera, Image* readback)
{
    return m_impl->Render(mesh, camera, readback);
}

bool Renderer::Render(const MeshData& mesh, const MaterialDesc& material, const Camera& camera, Image* readback)
{
    return m_impl->Render(mesh, material, camera, readback);
}

bool Renderer::Render(const Scene& scene, Image* readback)
{
    return m_impl->Render(scene, readback);
}

bool Renderer::Render(const Scene& scene, const Camera& camera, Image* readback)
{
    return m_impl->Render(scene, camera, readback);
}

bool Renderer::Render(const Canvas& canvas, Image* readback)
{
    return m_impl->Render(canvas, readback);
}

bool Renderer::Render(const Scene& scene, const Canvas& overlay, Image* readback)
{
    return m_impl->Render(scene, overlay, readback);
}

bool Renderer::Render(const Scene& scene, const Camera& camera, const Canvas& overlay, Image* readback)
{
    return m_impl->Render(scene, camera, overlay, readback);
}

bool Renderer::Render(const Scene& scene, const Camera& camera, const std::vector<RendererDrawDesc>& draws,
    const Canvas* overlay, Image* readback)
{
    return m_impl->Render(scene, camera, draws, overlay, readback);
}

bool Renderer::Render(const Scene& scene, const std::vector<RendererDrawDesc>& draws, const Canvas* overlay, Image* readback)
{
    return m_impl->Render(scene, draws, overlay, readback);
}

bool Renderer::RenderToTexture(const Scene& scene, const Camera& camera, RHI::TextureHandle target, RHI::ResourceState before)
{
    return m_impl->RenderToTexture(scene, camera, target, before);
}

bool Renderer::RenderToTexture(const Scene& scene, const Camera& camera, const std::vector<RendererDrawDesc>& draws,
    RHI::TextureHandle target, RHI::ResourceState before)
{
    return m_impl->RenderToTexture(scene, camera, target, before, &draws);
}

bool Renderer::CaptureEnvironment(const Scene& scene, Math::float3 position, uint32_t faceSize,
    float nearPlane, float farPlane, HdrImage& result, bool includeEmitters, HdrImage* visibility)
{
    return m_impl->CaptureEnvironment(scene, position, faceSize, nearPlane, farPlane, result, includeEmitters, visibility);
}

RHI::IDevice& Renderer::GetDevice() const
{
    return m_impl->GetDevice();
}

const Platform::ProfilerTimingSnapshot& Renderer::GetProfilerTimingSnapshot() const
{
    return m_impl->GetProfilerTimingSnapshot();
}

const RendererStatistics& Renderer::GetLastRenderStatistics() const
{
    return m_impl->GetLastRenderStatistics();
}

RHI::FenceHandle Renderer::GetLastRenderCompletion() const
{
    return m_impl->GetLastRenderCompletion();
}

bool Renderer::SetClearColor(Math::float4 value)
{
    return m_impl->SetClearColor(value);
}

bool Renderer::SetVSync(bool value)
{
    return m_impl->SetVSync(value);
}

void Renderer::InvalidateHistory()
{
    m_impl->InvalidateHistory();
}

bool Renderer::SetLighting(const LightingConfig& value)
{
    return m_impl->SetLighting(value);
}

bool Renderer::SetLightingEnabled(bool value)
{
    return m_impl->SetLightingEnabled(value);
}

bool Renderer::SetShadowsEnabled(bool value)
{
    return m_impl->SetShadowsEnabled(value);
}

bool Renderer::SetHdrEnabled(bool value)
{
    return m_impl->SetHdrEnabled(value);
}

bool Renderer::SetExposure(float value)
{
    return m_impl->SetExposure(value);
}

bool Renderer::SetProfilerVisible(bool value)
{
    return m_impl->SetProfilerVisible(value);
}

bool Renderer::SetReadbackEnabled(bool value)
{
    return m_impl->SetReadbackEnabled(value);
}

bool Renderer::AddExtension(std::unique_ptr<RenderExtension> extension)
{
    return m_impl->AddExtension(std::move(extension));
}

const RendererConfig& Renderer::GetConfig() const
{
    return m_impl->GetConfig();
}

bool Renderer::SetConfig(const RendererConfig& config)
{
    return m_impl->SetConfig(config);
}

bool Renderer::SetShaders(const RendererShaderDesc& shaders)
{
    return m_impl->SetShaders(shaders);
}

RendererShaderDesc Renderer::GetShaders() const
{
    return m_impl->GetShaders();
}
}


namespace dyf::RHI
{
std::uint64_t ProfilerAccess::Source(const dyf::Renderer& renderer) noexcept { return renderer.GetLiveSourceId(); }
}
