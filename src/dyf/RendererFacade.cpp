#include "../Platform/Profiler/RhiProfiler.h"
#include "RendererInternal.h"
#include "dyf/RenderExtension.h"
#include <utility>

namespace dyf
{
// 자기 자신과 장치·창·설정을 Impl에 전달한다. m_impl이 내부 구현을 소유하지만 전달받은 device 참조의 소유권은 여기서 가져오지 않는다.
Renderer::Renderer(RHI::IDevice& device, const void* window, const RendererConfig& config)
    : m_impl(std::make_unique<Impl>(*this, device, window, config))
{
}

// Impl 소멸자를 호출하게 해 라이브 등록과 GPU 자원을 정리한다. ownedDevice가 있으면 Impl이 소유한 장치도 이후 소멸한다.
Renderer::~Renderer() = default;
// 실제 적용 설정과 라이브 source ID를 공개/내부 조회에 연결한다. GetConfig가 반환하는 예약된 요청과 현재 적용 상태는 다를 수 있다.
const RendererConfig& Renderer::GetAppliedConfig() const { return m_impl->config; }
Platform::LiveId Renderer::GetLiveSourceId() const noexcept { return m_impl->liveId; }
void Renderer::InitializeLiveProfiling() noexcept { m_impl->InitializeLiveProfiling(); }
bool Renderer::PrepareLiveFrame(Platform::LiveFrameKey frame) noexcept { return m_impl->PrepareLiveFrame(frame); }
void Renderer::PublishLiveGeometryAvailability(bool custom) noexcept { m_impl->PublishLiveGeometryAvailability(custom); }

// Mesh 편의 호출은 내부에서 임시 Scene과 기본 방향광을 구성한다. Scene 호출은 사용자가 등록한 광원 구성 그대로 RenderScene으로 간다.
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

// Canvas 전용 경로로 전달한다. Scene 그래프와 Scene용 프로파일러 HUD를 자동으로 그리는 호출은 아니다.
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

// 객체별 RHI 입력을 Scene 객체 순서로 전달한다. 입력 개수·상수 크기·표준 지오메트리 호환 검사는 내부 RenderScene이 맡는다.
bool Renderer::Render(const Scene& scene, const Camera& camera, const std::vector<RendererDrawDesc>& draws,
    const Canvas* overlay, Image* readback)
{
    return m_impl->Render(scene, camera, draws, overlay, readback);
}

bool Renderer::Render(const Scene& scene, const std::vector<RendererDrawDesc>& draws, const Canvas* overlay, Image* readback)
{
    return m_impl->Render(scene, draws, overlay, readback);
}

// 외부 소유 텍스처와 시작 자원 상태를 내부 오프스크린 경로에 전달한다.
bool Renderer::RenderToTexture(const Scene& scene, const Camera& camera, RHI::TextureHandle target, RHI::ResourceState before)
{
    return m_impl->RenderToTexture(scene, camera, target, before);
}

// 정적 환경 캡처의 여섯 방향 렌더링·readback·설정 복원 처리를 EnvironmentCapture 구현으로 위임한다.
bool Renderer::CaptureEnvironment(const Scene& scene, Math::float3 position, uint32_t faceSize,
    float nearPlane, float farPlane, HdrImage& result, bool includeEmitters, HdrImage* visibility)
{
    return m_impl->CaptureEnvironment(scene, position, faceSize, nearPlane, farPlane, result, includeEmitters, visibility);
}

// 렌더러가 사용하는 장치에 대한 참조를 빌려 준다. 반환값을 얻었다고 호출자가 그 장치를 삭제할 소유권을 얻는 것은 아니다.
RHI::IDevice& Renderer::GetDevice() const
{
    return m_impl->GetDevice();
}

// 기존에 게시한 시간 스냅샷을 읽는다. 여기서 새 GPU 측정이나 완료 대기를 시작하지 않는다.
const Platform::ProfilerTimingSnapshot& Renderer::GetProfilerTimingSnapshot() const
{
    return m_impl->GetProfilerTimingSnapshot();
}

// 최근 Scene 호출의 메인·그림자 제출 통계를 읽는다. 해당 호출이 draw를 제출하지 못했다면 0이며, 별도 Surface/투명 패스와 Canvas/HUD 및 GPU가 고른 가시 개수는 이 카운터에 포함하지 않는다.
const RendererStatistics& Renderer::GetLastRenderStatistics() const
{
    return m_impl->GetLastRenderStatistics();
}

// 최근 draw 제출의 펜스를 반환한다. 이후 readback/Present가 실패해도 이미 제출한 작업의 완료 표식은 남는다.
RHI::FenceHandle Renderer::GetLastRenderCompletion() const
{
    return m_impl->GetLastRenderCompletion();
}

// 이하 단일 설정 함수는 내부에서 최신 요청 설정을 복사해 해당 항목만 바꾼다. 즉시 GPU 상태를 바꾸지 않고 다음 Render에서 적용하도록 예약한다.
bool Renderer::SetClearColor(Math::float4 value)
{
    return m_impl->SetClearColor(value);
}

bool Renderer::SetVSync(bool value)
{
    return m_impl->SetVSync(value);
}

// 카메라 컷·애니메이션 탐색 등으로 이전 프레임이 현재와 연결되지 않을 때 재투영 및 가시성 이력을 무효화한다.
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

// unique_ptr를 이동하여 확장 모듈을 Impl에 넘긴다. 빈 입력이나 열린 창 프레임이면 등록을 거부하며, 실패하더라도 전달한 모듈의 소유권을 호출자에게 되돌려주지는 않는다.
bool Renderer::AddExtension(std::unique_ptr<RenderExtension> extension)
{
    return m_impl->AddExtension(std::move(extension));
}

// 적용 대기 중인 요청이 있으면 그 요청을 읽는다. 현재 GPU 경로의 설정을 확인할 때는 GetAppliedConfig를 사용한다.
const RendererConfig& Renderer::GetConfig() const
{
    return m_impl->GetConfig();
}

// 전체 설정의 검증·예약을 위임한다. true는 요청을 받아들였다는 뜻이고 자원 재생성까지 성공했다는 뜻은 아니다.
bool Renderer::SetConfig(const RendererConfig& config)
{
    return m_impl->SetConfig(config);
}

// 입력 셰이더 바이트를 내부 저장소에 복사하고 다음 프레임 적용을 예약한다.
bool Renderer::SetShaders(const RendererShaderDesc& shaders)
{
    return m_impl->SetShaders(shaders);
}

// 반환 구조체의 바이트 포인터와 진입점은 내부의 안정된 읽기 뷰를 참조한다. 다음 설정 적용 이후까지 보관하지 않는다.
RendererShaderDesc Renderer::GetShaders() const
{
    return m_impl->GetShaders();
}
}


namespace dyf::RHI
{
// RHI 프로파일러가 Renderer의 비공개 source ID를 얻는 제한된 연결점이다. device·source·frame을 이어 같은 출처의 이벤트를 추적한다.
std::uint64_t ProfilerAccess::Source(const dyf::Renderer& renderer) noexcept { return renderer.GetLiveSourceId(); }
bool ProfilerAccess::GetCurrentNoAreaProof(const dyf::Renderer& renderer,BufferHandle lights) noexcept
{
    const auto* state=renderer.m_impl.get();
    return state && lights && state->frameNoAreaLights && lights==state->lightingBuffer;
}
}
