#include "RendererInternal.h"
#include "dyf/RenderExtension.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "BasicEnvironmentResources.h"
#include "BasicLightingResources.h"
#include "GpuVisibility.h"
#include <algorithm>
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
    return CollectRequirements().RequiresDepth();
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

// 이력 대응 확인→확장 카메라 준비→GPU 가시성→기본 조명 대체→공유 지오메트리→Lighting/Effects 준비 순서로 이번 프레임 입력을 구성한다.
bool Renderer::Impl::PrepareFrameResources(RenderFrame& frame,RHI::ICommandList& uploads)
{
    const auto& scene=frame.scene;
    auto& camera=frame.camera;
    const bool active=UsesAdvancedRendering();
    const auto& target=frame.color.texture->GetDesc();
    if(active) // 고급 경로에서만 모션/시간 누적의 대응 확인과 현재 행렬 후보를 수집한다.
    {
        const std::weak_ptr<Scene> owner=scene.m_lifetime;
        // 이전 모델 행렬과 현재 장면의 대응이 끊겼을 때만 자동으로 이력을 버린다.
        // 카메라 이동·회전·투영 변화는 재투영으로 처리한다. 호출자가 의미상
        // 불연속인 카메라 컷을 적용할 때는 기존 InvalidateHistory()를 호출한다.
        const bool cut=!frameHistoryValid || historyScene.expired() || historyScene.owner_before(owner) ||
            owner.owner_before(historyScene) || previousModels.size()!=scene.GetEntityCount();
        if(cut) {InvalidateFrameHistory();visibilityKeys.clear();} // 장면 대응이 끊겼으면 재투영 이력과 GPU LOD 키를 버린다.
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
    if(!PrepareCpuBatches(scene,camera,uploads,frame.draws,(frame.requirements.SurfaceOutputMask()&4u)!=0)) return false;
    if(shaderSources.bytes[MeshFragment].empty()) // 기본 fragment를 쓰는 경우에만 기본 조명 자원을 보충한다.
    {
        // Basic material paging uses neutral environment inputs even when an
        // extension provides the frame environment. Upload them with frame data.
        if(!frame.requirements.providesEnvironment || (UsesBindlessMaterials() && !active))
        {
            if(!basicEnvironment) basicEnvironment=std::make_unique<BasicEnvironmentResources>(); // 기본 환경 도우미가 없을 때만 지연 생성한다.
            if(!basicEnvironment->Prepare(*device,uploads)) return false; // 기본 환경의 버퍼·중립 배열 텍스처 준비 실패를 호출부로 전달한다.
            if(!frame.requirements.providesEnvironment)
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
    depthGeometryUnchanged=stockDrawInputs && UsesStockGeometry() && UsesAdvancedRendering() &&
        frame.requirements.RequiresDepth();
    if(depthGeometryUnchanged)
    {
        depthGeometrySnapshot.Capture(frame.geometry);
        depthGeometryView=frame.camera.view;depthGeometryProjection=frame.camera.projection;
    }
    // 등록 순서를 유지하면서 조명 단계 전체를 먼저, 효과 단계 전체를 다음으로 준비한다. 모듈 하나라도 실패하면 프레임 준비를 중단한다.
    for(auto stage:{RenderPreparation::Lighting,RenderPreparation::Effects})
        for(auto& extension:extensions)
            if(extension->GetPreparationStage()==stage)
            {
                if(!extension->Prepare(frame,uploads))
                {
                    std::fprintf(stderr,"dyf: Render module %s preparation failed.\n",extension->GetName());
                    return false;
                }
                if(depthGeometryUnchanged && (!depthGeometrySnapshot.Matches(frame.geometry) ||
                    !SameDepthCamera(depthGeometryView,depthGeometryProjection,frame.camera)))depthGeometryUnchanged=false;
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
    historyScene=frame.scene.m_lifetime;
    frameHistoryValid=true;++frameIndex;
}
}
