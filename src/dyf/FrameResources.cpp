#include "RendererInternal.h"
#include "dyf/RenderExtension.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "BasicEnvironmentResources.h"
#include "BasicLightingResources.h"
#include "GpuVisibility.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dyf
{
// 고급 설정·HDR·표준 셰이더 지오메트리 조건을 동시에 만족할 때만 고급 렌더 경로를 사용한다.
bool Renderer::Impl::UsesAdvancedRendering() const
{
    return config.advanced.enabled && config.enableHdrRendering && UsesStockGeometry();
}

// 등록 모듈과 차폐 제거의 합산 요구를 조회한다. 단순히 고급 설정이 켜졌다는 이유만으로 표면 패스가 필요하다고 가정하지 않는다.
bool Renderer::Impl::NeedsSurfacePass() const
{
    return CollectRequirements().needsSurface;
}

// 주변 형상에 가려 주변광이 덜 닿는 정도를 나타내는 값(AO)의 텍스처 연결을 비운다.
// 재투영 이력도 무효화해 모든 확장에 알리며, release=true이면 GPU 가시성·기본 조명 자원과 장면·행렬 이력도 해제한다.
void Renderer::Impl::InvalidateFrameHistory(bool release)
{
    frameHistoryValid=false;frameAmbientOcclusion=nullptr;
    for(auto& extension:extensions) extension->InvalidateHistory(release);
    if(release) // 강한 해제 요청이면 가시성/조명 자원과 장면/행렬 이력을 함께 비운다.
    {
        gpuVisibility.reset();visibilityActive=false;visibilityKeys.clear();visibilityScene.reset();
        basicLighting.reset();basicEnvironment.reset();frameLighting={};
        historyScene.reset();previousModels.clear();pendingModels.clear();frameIndex=0;
    }
}

// 카메라 컷 감지→확장 카메라 준비→GPU 가시성→기본 조명 대체→공유 지오메트리→Lighting/Effects 준비 순서로 이번 프레임 입력을 구성한다.
bool Renderer::Impl::PrepareFrameResources(RenderFrame& frame,RHI::ICommandList& uploads)
{
    const auto& scene=frame.scene;
    auto& camera=frame.camera;
    const bool active=UsesAdvancedRendering();
    const auto& target=frame.color.texture->GetDesc();
    if(active) // 고급 경로에서만 모션/시간 누적을 위한 컷 감지와 현재 행렬 후보를 수집한다.
    {
        const std::weak_ptr<Scene> owner=scene.m_lifetime;
        // 이력이 무효이거나 장면 소유자가 만료/교체되거나 객체 수가 이전 모델 행렬 수와 다르면 카메라 컷이다.
        // 두 벡터의 대응 성분을 곱해 더하는 연산(내적)으로 현재/이전 시선 방향의 변화도 검사한다.
        // 이동 거리 제곱이 16을 넘거나, 현재/이전 시선 방향의 내적이 0.8 미만이거나, 투영 행렬의 지정 성분 차이가 0.05를 넘어도 컷으로 처리한다.
        bool cut=!frameHistoryValid || historyScene.expired() || historyScene.owner_before(owner) ||
            owner.owner_before(historyScene) || previousModels.size()!=scene.GetEntityCount();
        cut=cut || Math::LengthSquared(camera.position-previousCameraPosition)>16.0f;
        float facing=0;
        for(uint32_t i:{2u,6u,10u}) facing+=camera.view.m[i]*previousCameraView.m[i];
        cut=cut || facing<.8f;
        for(uint32_t i:{0u,5u,10u,14u})
            cut=cut || std::fabs(camera.projection.m[i]-previousCameraProjection.m[i])>.05f;
        if(cut) {InvalidateFrameHistory();visibilityKeys.clear();} // 카메라 컷이나 장면 불연속이면 재투영 이력과 GPU LOD 키를 버린다.
        // 확장 모듈이 지터 등으로 카메라를 수정하기 전 카메라와 현재 객체 행렬을 후보로 저장한다. 제출 실패 시 이를 previous 이력으로 확정하지 않는다.
        pendingCameraView=camera.view;pendingCameraProjection=camera.projection;
        pendingModels.resize(scene.GetEntityCount());
        for(uint32_t i=0;i<scene.GetEntityCount();++i)
            pendingModels[i]=scene.GetTransform(static_cast<EntityID>(i));
    }
    else InvalidateFrameHistory(); // 고급 경로를 사용하지 않는 프레임은 이전 고급 효과 이력을 이어 쓰지 않는다.
    // 마지막 확정 이력을 확장에 전달한다. frameHistoryValid=false인 경우 행렬 값이 남아 있더라도 유효한 재투영 표본으로 사용하면 안 된다.
    frame.history={previousCameraView,previousCameraProjection,frameIndex,frameHistoryValid};
    for(auto& extension:extensions)
        if(!extension->PrepareCamera(frame)) return false; // 확장 카메라 준비 실패면 뒤의 가시성/자원 준비를 중단한다.
    if(!PrepareVisibility(scene,camera,uploads,target.width,target.height,frame.draws)) return false; // GPU 가시성 입력 준비/업로드 기록 실패를 호출부에 전달한다.
    if(shaderSources.bytes[MeshFragment].empty()) // 기본 fragment를 쓰는 경우에만 기본 조명 자원을 보충한다.
    {
        if(!frame.requirements.providesEnvironment) // 환경광 공급 모듈이 없으면 기본 환경 자원을 사용한다.
        {
            if(!basicEnvironment) basicEnvironment=std::make_unique<BasicEnvironmentResources>(); // 기본 환경 도우미가 없을 때만 지연 생성한다.
            if(!basicEnvironment->Prepare(*device,uploads)) return false; // 기본 환경의 상수·프로브 데이터·프로브 개수 버퍼 생성 또는 업로드 기록이 실패하면 false를 반환하고 프레임 입력 구성을 중단한다.
            frame.lighting.environment=basicEnvironment->GetResources(defaultMaterialTextures[2]);
        }
        if(!frame.requirements.providesLocalLights) // 지역광 공급 모듈이 없으면 기본 전체 광원 목록을 만든다.
        {
            if(!basicLighting) basicLighting=std::make_unique<BasicLightingResources>(); // 기본 지역광 도우미가 없을 때만 만든다.
            if(!basicLighting->Prepare(*device,uploads,scene,camera, // 지역광 버퍼 준비 실패면 비어 있는 바인딩으로 계속하지 않는다.
                config.lighting.enabled && config.lighting.direct && !lodDebugActive)) return false;
            frame.lighting.clusterLights.buffer=basicLighting->GetLights();
            frame.lighting.clusterList.buffer=basicLighting->GetList();
            frame.lighting.clusterConstants.buffer=basicLighting->GetConstants();
        }
    }
    UpdateFrameLighting(frame);
    PrepareRenderGeometry(frame);
    // 등록 순서를 유지하면서 조명 단계 전체를 먼저, 효과 단계 전체를 다음으로 준비한다. 모듈 하나라도 실패하면 프레임 준비를 중단한다.
    for(auto stage:{RenderPreparation::Lighting,RenderPreparation::Effects})
        for(auto& extension:extensions)
            if(extension->GetPreparationStage()==stage && !extension->Prepare(frame,uploads)) // 현재 단계 모듈의 Prepare가 실패하면 이름을 보고하고 중단한다.
            {
                std::fprintf(stderr,"dyf: Render module %s preparation failed.\n",extension->GetName());
                return false;
            }
    frameAmbientOcclusion=frame.ao.texture;
    frameLighting=frame.lighting;
    return true;
}

// 제출 성공 뒤 확장 모듈에 Commit을 알리고 현재 모델·카메라를 다음 프레임의 이전 상태로 보관한다. 이는 GPU 완료를 기다리는 함수가 아니다.
void Renderer::Impl::CommitFrameResources(const RenderFrame& frame)
{
    for(auto& extension:extensions) extension->CommitFrame(frame);
    if(!UsesAdvancedRendering()) return; // 고급 경로가 아니면 확장 Commit 이후 카메라/모델 이력 갱신은 생략한다.
    previousModels.swap(pendingModels);
    previousViewProjection=frame.camera.projection*frame.camera.view;
    previousCameraView=pendingCameraView;previousCameraProjection=pendingCameraProjection;
    previousCameraPosition=frame.camera.position;historyScene=frame.scene.m_lifetime;
    frameHistoryValid=true;++frameIndex;
}
}
