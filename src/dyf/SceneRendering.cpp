#include "../Platform/Profiler/CollectorInternal.h"
#include "../Platform/Profiler/RhiProfiler.h"
#include "RendererInternal.h"
#include "OutputPresentation.h"
#include "dyf/RHI/ICommandList.h"
#include "RendererDiagnostics.h"
#include "BasicEnvironmentResources.h"
#include "BasicLightingResources.h"
#include "GpuVisibility.h"
#include "dyf/Canvas.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include "dyf/Platform/Profiler.h"
#include "dyf/RHI/Readback.h"
#include "dyf/RHI/IDevice.h"
#include <algorithm>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace dyf;

// 외부 소유 2D 출력의 형식·용도·시작 상태를 검증한 후 같은 RenderScene으로 보낸다. 창의 획득/표시와 HUD/overlay 없이 ShaderResource 상태로 남긴다.
bool Renderer::Impl::RenderToTexture(const Scene& scene,const Camera& camera,RHI::TextureHandle target,RHI::ResourceState before,
    const std::vector<RendererDrawDesc>* draws)
{
    lastStatistics={};
    if(!target) return RendererFailure("RenderToTexture requires a target."); // 외부 출력 텍스처가 없으면 거부한다.
    const auto& desc=target->GetDesc();
    const auto usage=RHI::TextureUsage::RenderTarget|RHI::TextureUsage::ShaderResource;
    if(!desc.width || !desc.height || desc.depthOrArraySize!=1 || desc.mipLevels!=1 || // 크기/단일 2D/단일 mip/렌더와 샘플링 용도/RGBA·BGRA8 형식 계약을 벗어나면 거부한다.
        (desc.usage&usage)!=usage || !RHI::IsReadbackFormat(desc.format))
        return RendererFailure("RenderToTexture requires a single-mip 2D RGBA/BGRA8 renderable sampled texture.");
    if(before!=RHI::ResourceState::Undefined && before!=RHI::ResourceState::ShaderResource && // 외부 출력의 시작 상태는 Undefined/ShaderResource/RenderTarget만 받는다.
        before!=RHI::ResourceState::RenderTarget)
        return RendererFailure("RenderToTexture before state must be Undefined, ShaderResource or RenderTarget.");
    return RenderScene(scene,&camera,nullptr,nullptr,draws,target,before);
}

// Scene/카메라/overlay/사용자 draw/출력 조합을 하나의 프레임 경로로 처리한다. selectedOutput이 있으면 오프스크린, 없으면 창 출력이다.
bool Renderer::Impl::RenderScene(const Scene& scene, const Camera* selectedCamera, const Canvas* overlay, Image* readback,
    const std::vector<RendererDrawDesc>* draws,RHI::TextureHandle selectedOutput,RHI::ResourceState before)
{
    frameOpaqueDepthComplete=false;frameNoAreaLights=false;depthGeometryUnchanged=false;depthProtectedInputs.clear();
    // main은 사용자 draw 배열의 존재만으로 라이브 제어 가용성을 먼저 게시한다. 실제 표준 호환 판정은 설정 적용 뒤 아래에서 따로 수행한다.
    InitializeLiveProfiling();PublishLiveGeometryAvailability(draws!=nullptr);
    // 장치 ID와 Renderer source ID에 현재 프레임을 묶는다. 이 문맥이 이후 업로드·패스·제출 이벤트의 출처를 연결한다.
    Platform::LiveFrameScope liveFrame(Platform::FindLiveObject(device),liveId);
    device->PrepareLiveControls(liveFrame.GetKey());
    Platform::SetLiveObjectGauge(liveFrame.GetKey(),{Platform::LiveValueState::Ready,scene.GetEntityCount()});
    // 정상/실패 반환 모두에서 남은 GPU scope를 빈 완료 핸들 경로로 넘긴다. 실제 draw 제출에 연결된 scope는 아래에서 completion과 함께 별도로 넘긴다.
    struct LivePending {Impl& r;Platform::LiveFrameKey key;~LivePending(){r.m_profiler.SubmitGpuScopes({},key);}} livePending{*this,liveFrame.GetKey()};
    try
    {
        // 이번 프레임을 끝까지 확정하지 못하면 아틀라스 재사용 상태를 무효화한다. 부분적으로 기록/제출한 결과를 완성된 캐시라고 남기지 않는다.
        struct ShadowFrame {
            Impl& renderer;
            bool committed=false;
            ~ShadowFrame() {
                if(!committed) { // 프레임을 정상 확정하지 못하면 그림자 아틀라스 재사용 후보를 폐기한다.
                    renderer.shadowAtlasCache.valid=false;
                    renderer.shadowAtlasCache.pending=false;
                }
            }
        } shadowFrame{*this};
        lastStatistics={};
        DY_PROFILE_CPU_ZONE_NAMED("Renderer::Impl::RenderScene");
        if(!PrepareLiveFrame(liveFrame.GetKey()))return false; // 프레임 경계의 설정 적용 실패면 장면 기록을 시작하지 않는다.
        if(draws && draws->size() != scene.GetEntityCount()) return RendererFailure("Draw input count must match the scene."); // 사용자 draw 배열 길이가 Scene 객체 수와 다르면 객체별 입력을 해석할 수 없다.
        // 사용자 입력이 없거나 모든 입력이 표준 기하이며 추가 자원·상수가 없는지 확인한다. 이 결과와 현재 셰이더 호환성을 합쳐 실제 렌더 경로를 정한다.
        const bool stockDraws=!draws || std::all_of(draws->begin(),draws->end(),[](const RendererDrawDesc& draw) {
            return draw.stockGeometry && draw.vertexResources.empty() && draw.inlineConstants.empty();
        });
        // 이 값은 뒤의 배칭·컬링 경로가 사용한다. main에서는 여기서 라이브 제어의 가용성을 다시 게시하지 않는다.
        stockDrawInputs=stockDraws && UsesStockGeometry();
        if(config.advanced.enabled && config.enableHdrRendering && (!stockDraws || !UsesStockGeometry())) // 고급 HDR에 비표준 지오메트리/모션 입력을 섞은 요청은 명시적으로 실패한다.
            return RendererFailure("Advanced raster passes require stock-compatible geometry with matching surface and motion inputs.");
        const auto requirements=CollectRequirements();
        if(!ValidateModules(requirements)) return false; // 확장 공급 충돌이나 필수 surface 부재가 있으면 중단한다.
        if(draws) for(const auto& draw : *draws) // 사용자 입력이 있을 때만 각 객체의 추가 상수 크기를 검사한다.
            if(draw.inlineConstants.size() != shaderSources.additionalConstantBytes) // 셰이더가 선언한 추가 상수 바이트 수와 다르면 렌더를 거부한다.
                return RendererFailure("Draw constants do not match the selected shader layout.");
        Platform::CpuTimer acquireTimer;
        Platform::ProfilerTimingSample timing;
        if(readback && !config.allowReadback) return RendererFailure("Renderer readback was not enabled."); // 캡처를 요청했지만 실제 적용 설정이 허용하지 않으면 실패한다.
        RHI::IDevice* nativeDevice = device;
        if(!selectedOutput && !nativeDevice->BeginFrame()) // 창 프레임을 확보하지 못하면 장치 손실인지 정상적인 프레임 생략인지 나눈다.
        {
            m_profiler.AddAcquireWait(acquireTimer.ElapsedMilliseconds());
            if(nativeDevice->IsLost())return RendererFailure("RHI device was lost."); // 장치 손실은 정상 생략으로 취급하지 않고 실패를 반환한다.
            if(readback)*readback={}; // 정상 acquire 생략은 새 이미지가 없으므로 readback 결과를 비운다.
            // No commands or atlas writes occurred on an ordinary acquire skip.
            shadowFrame.committed=true;
            livePreparedFrame=0;liveFrame.SetOutcome(Platform::LiveOutcome::Skipped);
            return true;
        }
        if(!selectedOutput) framePending=true; // 창 프레임을 확보한 경우에만 아직 Present로 닫지 않은 상태를 표시한다.
        if(!selectedOutput) modelFramePending=false; // Model의 선행 확보를 이 함수의 FrameEnd로 넘긴다.
        // 창 획득 뒤 어느 지점에서 돌아가도 열린 프레임을 닫으려는 보호 객체다. 오프스크린은 새 창 프레임을 열지 않는다.
        FrameEnd frameEnd{*nativeDevice,framePending};
        timing.cpuAcquireMilliseconds=selectedOutput?0.0:m_profiler.TakeAcquireWait()+acquireTimer.ElapsedMilliseconds();
        Platform::CpuTimer prepareTimer;
        if(!m_environmentCapture)m_profiler.PollGpuSamples(); // 창·텍스처 Scene 경로에서 이전 제출의 GPU 표본을 polling한다. 이 조회가 모든 GPU 작업을 동기 대기하는 것은 아니다.
        auto* windowOutput=selectedOutput ? nullptr : nativeDevice->GetBackBuffer();
        auto* output=selectedOutput ? selectedOutput : windowOutput;
        // 고정 장면 해상도는 별도 타깃에 그리고 창으로 비율 유지 복사한다. overlay/HUD는 최종 창 크기를 따라 그린다.
        const bool fixedOutput=!selectedOutput && config.renderWidth && config.renderHeight;
        if(fixedOutput) // 고정 장면 해상도가 지정되었으면 별도 출력 타깃을 준비한다.
        {
            if(!outputPresentation) outputPresentation=std::make_unique<OutputPresentation>(*nativeDevice); // 고정 해상도 출력 관리자를 처음 필요할 때 생성한다.
            if(!outputPresentation->Prepare(config.renderWidth,config.renderHeight,windowOutput->GetDesc().format)) // 고정 타깃 생성/크기 준비 실패면 현재 프레임을 중단한다.
                return RendererFailure("Fixed render output preparation failed.");
            output=outputPresentation->GetTarget();
        }
        // 외부/고정 출력은 셰이더 읽기 상태로, 일반 창 출력은 Present 상태로 끝난다. 시작 상태는 호출자 또는 출력 관리자의 실제 추적 상태를 사용한다.
        const auto outputBefore=fixedOutput ? outputPresentation->GetState() : before;
        const auto after=selectedOutput || fixedOutput ? RHI::ResourceState::ShaderResource : RHI::ResourceState::Present;
        Camera defaultCamera;
        const auto& targetDesc = output->GetDesc();
        if(!selectedCamera) defaultCamera.SetPerspective(targetDesc.width / static_cast<float>(targetDesc.height)); // 카메라를 생략했으면 실제 출력 비율로 기본 원근 카메라를 구성한다.
        Camera camera = selectedCamera ? *selectedCamera : defaultCamera;
        if(!PrepareGeometry(scene, nativeDevice)) return RendererFailure("Mesh upload failed."); // 메시 GPU 준비/업로드 실패면 장면 패스를 만들지 않는다.
        // 장면 업로드 이후 그림자 뷰/아틀라스를 계산하고 HDR/파이프라인/LOD를 준비한다. LOD는 실제 장면 타깃의 픽셀 크기를 기준으로 고른다.
        ShadowData shadows;
        BuildShadows(shadows,scene,camera,draws);
        if(config.enableHdrRendering && !PreparePostProcess(output)) // HDR이 요청되었는데 중간 출력/후처리 준비가 실패하면 중단한다.
            return RendererFailure("HDR pass creation failed.");
        if(!InitializeMesh(output,selectedOutput!=nullptr)) return RendererFailure("Mesh pipeline creation failed."); // 출력 형식에 맞는 메시 파이프라인 준비 실패면 중단한다.
        const auto* lodTarget=config.enableHdrRendering ? hdrTarget : output;
        SelectLods(scene,camera,lodTarget->GetDesc().width,lodTarget->GetDesc().height,draws);


        if(!SyncTextures(scene, nativeDevice)) return RendererFailure("Scene texture upload failed."); // Scene 텍스처 동기화 실패면 재질 상태를 확정하지 않는다.
        materialStates.resize(scene.Materials().size());
        UpdateMaterialStates(scene);
        auto* sceneTarget=config.enableHdrRendering ? hdrTarget : output;
        const bool advanced=UsesAdvancedRendering();
        const bool captureSky=m_environmentCapture && config.lighting.environment.background &&
            config.lighting.environment.radiance.IsValid();
        // 고급 렌더 경로·환경 캡처의 배경·가림 거리 캡처 중 하나라도 깊이를 읽으면 샘플링 가능한 깊이 타깃을 만든다. 같은 sampledDepth 값으로 그래프의 최종 상태도 ShaderResource로 정한다.
        const bool sampledDepth=advanced || captureSky || m_environmentCaptureVisibility;
        if(!EnsureDepthStencilTarget(nativeDevice,sceneTarget,sampledDepth) || // 메인 깊이 또는 그림자 깊이 타깃 생성 실패면 프레임 준비를 중단한다.
            !EnsureShadowDepthTarget(nativeDevice,shadows.columns,shadows.rows,shadows.resolution))
            return RendererFailure("Scene depth target creation failed.");
        // 이번 프레임의 Renderer·Scene·복사한 카메라와 자원 핸들을 확장 모듈이 읽을 묶음으로 연결한다. RenderFrame 자체가 Scene을 새로 소유하는 것은 아니다.
        RenderFrame renderFrame{owner,scene,camera};
        renderFrame.requirements=requirements;
        renderFrame.lodDebug=lodDebugActive;renderFrame.wireframe=wireframeActive;
        renderFrame.blackTexture=defaultMaterialTextures[2];
        renderFrame.environmentCapture=m_environmentCapture;
        renderFrame.draws=draws;renderFrame.output.texture=output;
        renderFrame.color.texture=config.enableHdrRendering ? hdrTarget : output;
        renderFrame.depth.texture=depthStencilTarget;renderFrame.shadow.texture=shadowDepthTarget;
        // draw 제출 성공 전 실패하면 고급 프레임 이력과 관련 자원을 무효화한다. 성공 시 committed를 켜서 이번 프레임 이력을 보존한다.
        struct FrameResources {
            Impl& renderer;
            bool committed=false;
            ~FrameResources() {if(!committed) renderer.InvalidateFrameHistory(true);} // draw 제출 성공으로 확정하지 못한 프레임은 이력과 보조 자원을 무효화한다.
        } frameResources{*this};
        // 획득한 GPU timestamp query를 지역 소유하고, 유효한 제출 펜스와 함께 프로파일러에 넘긴 경우에만 packet을 비워 소유권 이전을 표시한다.
        struct GpuSamples {
            RHI::IDevice& device;
            Platform::FrameProfiler::GpuSample packet;
            ~GpuSamples() {device.DestroyTimestampQuery(packet.query);device.DestroyTimestampQuery(packet.start);}
        } samples{*nativeDevice,{}};
        if(!m_environmentCapture) samples.packet.start=m_profiler.AcquireQuery(1); // 환경 캡처를 제외한 창·텍스처 Scene 시간 측정에 프레임 시작 timestamp query를 획득한다.

        {
            // 크기가 바뀐 조명·그림자·인스턴스 버퍼만 교체하며, 업로드·닫기·제출 성공이면 과거 자원을 해제하고 실패·예외면 새 자원을 버려 핸들·ready를 복원한다.
            struct FrameBuffers {
                Impl& r;
                RHI::BufferHandle light,shadow,instances,cpuInstances;
                bool lightReady,shadowReady,instancesReady,cpuInstancesReady,committed=false;
                explicit FrameBuffers(Impl& owner):r(owner),light(r.lightingBuffer),shadow(r.shadowMatrixBuffer),instances(r.instanceBuffer),
                    cpuInstances(r.cpuInstanceBuffer),lightReady(r.lightingReady),shadowReady(r.shadowReady),
                    instancesReady(r.instanceReady),cpuInstancesReady(r.cpuInstanceReady) {}
                ~FrameBuffers() {
                    const auto finish=[&](RHI::BufferHandle& current,RHI::BufferHandle previous,bool& ready,bool oldReady) {
                        if(committed) {if(current!=previous)r.device->DestroyBuffer(previous);} // 업로드 제출 성공이면 교체된 과거 버퍼를 해제한다.
                        else {if(current!=previous)r.device->DestroyBuffer(current);current=previous;ready=oldReady;} // 업로드 준비/제출 실패면 새 버퍼를 해제하고 과거 핸들/ready로 복구한다.
                    };
                    finish(r.lightingBuffer,light,r.lightingReady,lightReady);
                    finish(r.shadowMatrixBuffer,shadow,r.shadowReady,shadowReady);
                    finish(r.instanceBuffer,instances,r.instanceReady,instancesReady);
                    finish(r.cpuInstanceBuffer,cpuInstances,r.cpuInstanceReady,cpuInstancesReady);
                }
            } buffers(*this);
            // 업로드 명령 목록을 범위 소유자에 맡겨 예외나 조기 반환에서도 해제한다.
            RHI::ResourceScope resources(*nativeDevice);
            auto* frameDataCommand=nativeDevice->AcquireCommandList();
            if(!frameDataCommand) return RendererFailure("Frame command list acquisition failed."); // 업로드용 명령 목록 획득 실패를 보고한다.
            resources.Keep(frameDataCommand);
            if(samples.packet.start) { // 유효한 시작 쿼리가 있을 때만 reset/write 명령을 기록한다.
                frameDataCommand->ResetTimestamps(samples.packet.start,0,1);
                frameDataCommand->WriteTimestamp(samples.packet.start,0);
            }
            if(!PrepareFrameResources(renderFrame,*frameDataCommand)) // 카메라/확장/조명/가시성 자원 준비가 실패하면 업로드 제출을 하지 않는다.
                return RendererFailure("Frame resource preparation failed.");
            if(!UpdateShadowBuffer(nativeDevice,*frameDataCommand,shadows)) // 그림자 상수 업로드 기록 실패를 전달한다.
                return RendererFailure("Shadow constants upload recording failed.");
            if(!UpdateLightingBuffer(scene,camera,nativeDevice,*frameDataCommand)) // 조명 상수 업로드 기록 실패를 전달한다.
                return RendererFailure("Lighting constants upload recording failed.");
            if(!UpdateInstanceBuffer(scene,*frameDataCommand)) // 인스턴스 변환 업로드 기록 실패를 전달한다.
                return RendererFailure("Instance transforms upload recording failed.");
            if(!frameDataCommand->Close()) // 논리 업로드 명령 목록을 닫지 못하면 제출하지 않는다.
                return RendererFailure("Frame upload command list close failed.");
            if(!nativeDevice->PrepareCommandLists(&frameDataCommand,1,1)) // 네이티브 명령 준비가 실패하면 큐 제출 단계로 가지 않는다.
                return RendererFailure("Frame upload command list native preparation failed.");
            Platform::CpuTimer uploadSubmitTimer;
            // 앞서 닫고 네이티브 준비한 업로드 명령을 큐에 제출한다. 같은 큐의 뒤쪽 draw와 순서가 이어지며 여기서 GPU 완료를 기다리지는 않는다.
            const bool submitted=nativeDevice->Submit(&frameDataCommand,1);
            timing.cpuSubmitMilliseconds+=uploadSubmitTimer.ElapsedMilliseconds();
            if(!submitted) return RendererFailure("Frame upload submission failed."); // 큐가 업로드 제출을 받지 못하면 버퍼 교체/ready를 확정하지 않는다.
            // 업로드 제출 성공에 대해서만 자원 ready와 변경 바이트/revision을 확정한다. draw 제출 성공 여부는 아직 별도다.
            lightingReady=true;shadowReady=shadowMatrixBuffer!=nullptr;instanceReady=instanceBuffer!=nullptr;
            cpuInstanceReady=cpuInstanceBuffer!=nullptr;
            if(visibilityActive) gpuVisibility->CommitUploads(); // GPU 가시성 업로드가 있었으면 같은 제출 성공 기준으로 확정한다.
            if(basicEnvironment) basicEnvironment->CommitUploads(); // 기본 환경 자원을 사용했다면 그 업로드 준비도 확정한다.
            CommitFrameData();
            buffers.committed=true;
        }

        // 이하 패스 기록 중 생길 Canvas 자원 변화를 보호한다. draw 제출 성공에서 Commit한다.
        CanvasFrame canvasFrame(*this);
        recordingStatistics={};materialIndexRecorded=false;
        // 호출마다 자원 세트의 미사용 프레임 수를 하나씩 올리되 최대 동시 진행 프레임 수에서 멈춘다. 이번에 다시 쓰는 세트는 CachedResourceSet에서 0으로 되돌린다.
        const auto frameWindow=std::max(1u,device->GetDesc().maxFramesInFlight);
        for(auto& set:mainSets)if(set.unusedFrames<frameWindow)++set.unusedFrames; // 메인 자원 세트의 미사용 프레임 수를 보존 한도까지만 늘린다.
        for(auto& set:shadowSets)if(set.unusedFrames<frameWindow)++set.unusedFrames; // 그림자 자원 세트도 같은 보존 한도로 나이를 올린다.
        // 프레임 종료 시 오래 쓰지 않은 자원 세트를 정리한다. 빈 Scene은 모든 캐시를 미사용으로 표시해 빠르게 정리한다.
        struct CacheFrame {
            Impl& owner;
            bool empty;
            ~CacheFrame() {
                if(empty) { // Scene에 객체가 없으면 모든 세트를 오래 미사용한 것으로 표시해 정리한다.
                    for(auto& set:owner.mainSets)set.unusedFrames=UINT32_MAX;
                    for(auto& set:owner.shadowSets)set.unusedFrames=UINT32_MAX;
                }
                owner.PruneResourceSets(owner.mainSets);owner.PruneResourceSets(owner.shadowSets);
            }
        } cacheFrame{*this,scene.GetEntityCount()==0};
        // 설정 요청만으로 그림자를 사용했다고 하지 않고 파이프라인·뷰·깊이·상수·타깃 폭이 모두 준비되었는지 확인한다.
        const bool hasShadows = shadowPipeline && shadows.viewCount && shadowDepthTarget &&
            shadowMatrixBuffer && shadowDepthTarget->GetDesc().width;
        const bool cachedShadows=hasShadows && ReuseShadowAtlas(scene,shadows,draws);
        const bool drawShadows=hasShadows && !cachedShadows;
        frameNoAreaLights=frameNoAreaLights && stockDrawInputs && UsesStockGeometry() && !wireframeActive &&
            noAreaMaterialPipelines[0];
        using State = RHI::ResourceState;
        // 타깃을 시작/끝 상태와 함께 그래프로 가져오고 패스의 읽기/쓰기 의존성을 선언한다. 이 선언은 실제 명령 기록보다 먼저 이루어진다.
        RHI::RenderGraph graph;
        const auto ownedLighting=frameNoAreaLights
            ? graph.ImportBuffer("Renderer owned lighting constants",lightingBuffer,State::ConstantBuffer,State::ConstantBuffer)
            : RHI::RGResourceHandle{};
        if(frameNoAreaLights && !ownedLighting.IsValid())return RendererFailure("Lighting resource import failed.");
        const auto outputHandle = graph.ImportTexture("Output", output, outputBefore, after);
        if(!outputHandle.IsValid()) return RendererFailure("Output resource import failed."); // 출력 자원을 그래프로 가져오지 못하면 실패한다.
        auto* overlayTarget=fixedOutput ? windowOutput : output;
        const auto overlayHandle=fixedOutput
            ? graph.ImportTexture("Window output",windowOutput,State::Present,State::Present) : outputHandle;
        if(!overlayHandle.IsValid()) return RendererFailure("Window output resource import failed."); // 최종 창/overlay 출력 import 실패를 전달한다.
        const bool surfaceNeeded=requirements.needsSurface;
        // 이번에 실제 사용한 장면/출력 크기와 surface·cluster·그림자/재사용 경로를 통계에 기록한다. 요청 토글 값과 구분한다.
        recordingStatistics.renderWidth=sceneTarget->GetDesc().width;
        recordingStatistics.renderHeight=sceneTarget->GetDesc().height;
        recordingStatistics.outputWidth=overlayTarget->GetDesc().width;
        recordingStatistics.outputHeight=overlayTarget->GetDesc().height;
        recordingStatistics.surfacePrepass=surfaceNeeded;
        recordingStatistics.gpuOcclusion=false;
        recordingStatistics.clusteredLighting=renderFrame.lighting.clustered;
        recordingStatistics.shadowViewCount=hasShadows ? shadows.viewCount : 0;
        recordingStatistics.shadowViewResolution=hasShadows ? shadows.resolution : 0;
        recordingStatistics.directionalShadowDistance=hasShadows ? shadows.directionalDistance : 0;
        recordingStatistics.reusedShadowAtlas=cachedShadows;
        const auto depth = graph.ImportTexture("Depth", depthStencilTarget, depthStencilState,
            sampledDepth ? State::ShaderResource : State::DepthWrite);
        if(!depth.IsValid()) return RendererFailure("Scene depth resource import failed."); // 메인 깊이 import가 유효하지 않으면 패스를 구성하지 않는다.
        const auto color = config.enableHdrRendering
            ? graph.ImportTexture("HDR", hdrTarget, hdrState, State::ShaderResource) : outputHandle;
        if(!color.IsValid()) return RendererFailure("Scene color resource import failed."); // HDR 또는 직접 색 타깃 import 실패를 전달한다.
        const auto shadow = shadowDepthTarget
            ? graph.ImportTexture("ShadowDepth", shadowDepthTarget, shadowDepthState, State::ShaderResource)
            : RHI::RGResourceHandle{};
        if(shadowDepthTarget && !shadow.IsValid()) return RendererFailure("Shadow depth resource import failed."); // 그림자 깊이가 존재하는데 그래프 import가 실패하면 중단한다.
        renderFrame.output.resource=outputHandle;renderFrame.color.resource=color;
        renderFrame.depth.resource=depth;renderFrame.shadow.resource=shadow;
        renderFrame.hasShadows=hasShadows;
        // SetExecute 콜백에서 bool 실패를 예외로 바꿔 그래프 기록 실패까지 전달한다. 실패한 패스를 성공한 명령 목록처럼 제출하지 않기 위한 연결이다.
        const auto require = [](bool success) {
            if(!success) throw std::runtime_error("Renderer pass recording failed.");
        };
        if(visibilityActive && !gpuVisibility->AddPasses(graph)) // GPU 가시성 패스를 추가하지 못하면 간접 draw를 만들지 않는다.
            return RendererFailure("GPU visibility pass creation failed.");
        if(cpuBatchesActive && !cpuDraws.empty())
        {
            renderFrame.visibleInstances={cpuInstanceBuffer,graph.ImportBuffer("CPU visible instances",cpuInstanceBuffer,
                State::VertexBuffer,State::VertexBuffer)};
            if(!renderFrame.visibleInstances.resource.IsValid())return RendererFailure("CPU instance import failed.");
        }
        if(!requirements.providesLocalLights && basicLighting && !basicLighting->Import(graph,renderFrame.lighting)) // 지역광 공급 확장이 없는 경우 기본 지역광 자원을 그래프에 가져오며 실패는 전파한다.
            return RendererFailure("Basic local light resource import failed.");
        if(drawShadows) // 그림자가 준비되었지만 정적 아틀라스를 재사용할 수 없을 때만 새 그림자 패스를 기록한다.
            graph.AddPass("Shadow", RHI::RGRecordingMode::Independent).Write(shadow,State::DepthWrite)
                .SetExecute([&](RHI::ICommandList* commands) {
                    require(RecordShadowPass(scene,camera,shadows,*commands,draws));
                });
        // GPU visibility 패스/기본 조명 import 뒤에는 현재 graph의 리소스 핸들을 확장에도 연결할 수 있다.
        UpdateFrameLighting(renderFrame,true);
        if(!AddExtensionPasses(RenderStage::BeforeOpaque,renderFrame,graph)) return false; // 불투명 이전 확장 패스 구성 실패면 중단한다.
        auto& environment=renderFrame.lighting.environment;
        if(!environment.diffuse.resource.IsValid() && environment.diffuse.texture) // 텍스처는 있으나 그래프 핸들이 없으면 기본 확산 환경을 현재 그래프로 가져온다.
            environment.diffuse.resource=graph.ImportTexture("Basic environment diffuse",environment.diffuse.texture,State::ShaderResource,State::ShaderResource);
        if(!environment.specular.resource.IsValid() && environment.specular.texture) // 기본 반사 환경도 현재 그래프의 리소스로 연결한다.
            environment.specular.resource=graph.ImportTexture("Basic environment specular",environment.specular.texture,State::ShaderResource,State::ShaderResource);
        frameAmbientOcclusion=renderFrame.ao.texture;
        // 주변 형상에 가려 주변광이 덜 닿는 정도를 나타내는 값(AO)을 표면 패스에서 공급한다.
        if(surfaceNeeded) // 표면 패스가 필요하면 normal·albedo·AO 입력을 확인하고 차폐 후속 패스를 구성한다.
        {
            if(!renderFrame.normal.resource.IsValid() || !renderFrame.albedo.resource.IsValid() || !renderFrame.ao.resource.IsValid()) // 필수 표면 자원 중 하나라도 확장이 공급하지 않았으면 실패한다.
                return RendererFailure("The surface extension did not provide normal, albedo and AO resources.");
            // GPU 가시성·차폐 요청과 텍스처/샘플러 Compute 레이아웃 지원이 모두 있을 때만 HiZ 차폐를 구성한다.
            const bool effectiveOcclusion=visibilityActive && config.enableOcclusionCulling &&
                GpuVisibility::SupportsOcclusion(*nativeDevice);
            if(effectiveOcclusion)
            {
                if(!gpuVisibility->AddOcclusionPasses(graph,depthStencilTarget,depth,camera)) // 차폐 패스 구성 실패면 gpuOcclusion을 성공 사용으로 표시하지 않는다.
                    return RendererFailure("GPU occlusion pass creation failed.");
                recordingStatistics.gpuOcclusion=true;
            }
        }
        if(!AddExtensionPasses(RenderStage::OpaqueLighting,renderFrame,graph)) return false; // 불투명 조명 확장 패스 구성 실패를 전달한다.
        frameOpaqueDepthComplete=renderFrame.opaqueDepthComplete && depthGeometryUnchanged &&
            stockDrawInputs && UsesStockGeometry() && shaderSources.bytes[MeshFragment].empty() &&
            advanced && !wireframeActive && requirements.needsSurface && UsesSeparateTransparency() &&
            renderFrame.depth.texture==depthStencilTarget && renderFrame.depth.resource==depth && depthPrepassPipelines[0];
        // 색/깊이 쓰기와 조명·그림자·AO·인수 읽기 의존성을 선언해 그래프가 실행 순서와 전환을 정하게 한다.
        auto& mainPass = graph.AddPass("MainForward", RHI::RGRecordingMode::Independent);
        mainPass.Write(color, State::RenderTarget).Write(depth, State::DepthWrite);
        if(ownedLighting.IsValid())mainPass.Read(ownedLighting,State::ConstantBuffer);
        if(renderFrame.lighting.areaMatrix.resource.IsValid()) // 면적광 행렬 LUT가 그래프 자원이면 메인 패스 읽기 의존성을 추가한다.
            mainPass.Read(renderFrame.lighting.areaMatrix.resource,State::ShaderResource);
        if(renderFrame.lighting.areaAmplitude.resource.IsValid()) // 면적광 진폭 LUT 읽기 의존성을 추가한다.
            mainPass.Read(renderFrame.lighting.areaAmplitude.resource,State::ShaderResource);
        if(renderFrame.lighting.clusterLights.resource.IsValid()) // 지역광/클러스터 자원이 연결되었으면 목록과 상수의 읽기를 선언한다.
            mainPass.Read(renderFrame.lighting.clusterLights.resource,State::ShaderResource)
                .Read(renderFrame.lighting.clusterList.resource,State::ShaderResource)
                .Read(renderFrame.lighting.clusterConstants.resource,State::ConstantBuffer);
        if(visibilityActive) // GPU 가시성 출력은 정점 스트림과 간접 인수 읽기로 선언한다.
            mainPass.Read(gpuVisibility->GetInstancesHandle(),State::VertexBuffer)
                .Read(gpuVisibility->GetArgumentsHandle(),State::IndirectArgument);
        else if(cpuBatchesActive && !cpuDraws.empty())
            mainPass.Read(renderFrame.visibleInstances.resource,State::VertexBuffer);
        if(shadowDepthTarget) mainPass.Read(shadow, State::ShaderResource); // 그림자 깊이 텍스처가 있으면 메인에서 샘플링하는 의존성을 추가한다.
        if(environment.diffuse.resource.IsValid()) mainPass.Read(environment.diffuse.resource,State::ShaderResource); // 유효한 확산 환경 핸들이 있으면 읽기 의존성을 선언한다.
        if(environment.specular.resource.IsValid()) mainPass.Read(environment.specular.resource,State::ShaderResource); // 유효한 반사 환경 핸들이 있으면 읽기 의존성을 선언한다.
        if(surfaceNeeded) // 표면 패스가 만든 AO를 메인 조명이 읽는다고 선언한다.
            mainPass.Read(renderFrame.ao.resource,State::ShaderResource);
        mainPass.SetExecute([&](RHI::ICommandList* commands) {
            require(RecordMainPass(scene, camera, *commands, draws, output));
        });
        // 불투명 이후 배경·화면 효과·대기·투명·후처리 확장을 정해진 단계 순서로 추가한다.
        for(auto stage:{RenderStage::Background,RenderStage::AfterOpaque,RenderStage::Atmosphere,
            RenderStage::Transparency,RenderStage::PostProcess})
            if(!AddExtensionPasses(stage,renderFrame,graph)) return false; // 후속 확장 단계 하나라도 추가 실패하면 프레임을 중단한다.
        const bool toneMap=config.enableHdrRendering && !renderFrame.outputWritten;
        if(toneMap) // HDR인데 확장이 최종 출력을 아직 쓰지 않았으면 기본 ToneMap을 추가한다.
            graph.AddPass("ToneMap", RHI::RGRecordingMode::Independent).Read(renderFrame.color.resource,State::ShaderResource).Write(outputHandle,State::RenderTarget)
                .SetExecute([&,source=renderFrame.color.texture](RHI::ICommandList* commands) {
                    require(RecordToneMap(*commands,output,config.exposure,source));
                });
        if(fixedOutput && !outputPresentation->AddPass(graph,outputHandle,windowOutput)) // 고정 장면 타깃을 창으로 복사하는 패스 추가 실패를 전달한다.
            return RendererFailure("Fixed render presentation pass creation failed.");
        if(overlay) // overlay 입력이 있을 때만 최종 출력 위에 Canvas를 그린다.
            graph.AddPass("Overlay").Write(overlayHandle, State::RenderTarget)
                .SetExecute([&](RHI::ICommandList* commands) {
                    require(RecordCanvas(*overlay,*commands,overlayTarget,true,true,&preparedCanvases[0]));
                });
        std::optional<Canvas> profilerOverlay;
        if(!selectedOutput && config.enableProfilerHud) // 창 출력에서 HUD 표시가 켜진 경우만 패스를 추가한다.
        {
            profilerOverlay.emplace(BuildProfilerOverlay(overlayTarget->GetDesc().width,overlayTarget->GetDesc().height,
                config.profilerStartsExpanded));
            graph.AddPass("Profiler HUD").Write(overlayHandle, State::RenderTarget)
                .SetExecute([&](RHI::ICommandList* commands) {
#ifdef DY_RENDERDOC_ENABLED // RenderDoc 빌드에서 HUD 디버그 이벤트 시작을 기록한다.
                    commands->BeginDebugEvent("Profiler HUD");
#endif
                    require(RecordCanvas(*profilerOverlay,*commands,overlayTarget,true,true,&preparedCanvases[1]));
#ifdef DY_RENDERDOC_ENABLED // RenderDoc 빌드에서 HUD 디버그 이벤트를 닫는다.
                    commands->EndDebugEvent();
#endif
                });
        }
        frameOpaqueDepthComplete=frameOpaqueDepthComplete && depthGeometryUnchanged &&
            depthGeometrySnapshot.Matches(renderFrame.geometry) &&
            SameDepthCamera(depthGeometryView,depthGeometryProjection,renderFrame.camera);
        if(frameNoAreaLights)
        {
            // Any graph writer may replace the uploaded zero counts. Unknown
            // callbacks with a global barrier may also touch resources outside RG.
            for(uint32_t index=0;const auto* pass=graph.GetPass(index);++index)
            {
                if(pass->HasGlobalBarrier()) {frameNoAreaLights=false;break;}
                for(const auto& write:pass->GetWrites())if(write.handle==ownedLighting) {frameNoAreaLights=false;break;}
                if(!frameNoAreaLights)break;
            }
            if(frameOpaqueDepthComplete && !noAreaDepthPrepassPipelines[0])frameNoAreaLights=false;
        }
        struct FrameCommands
        {
            RHI::IDevice& device;
            std::vector<RHI::ICommandList*> lists;
            RHI::ICommandList* preparation=nullptr;
            ~FrameCommands() {
                if(preparation)device.DestroyCommandList(preparation);
                for(auto* commands:lists)device.DestroyCommandList(commands);
            }
        } frame{*nativeDevice,{}};
        frame.preparation=nativeDevice->AcquireCommandList();
        if(!frame.preparation)return RendererFailure("Pass preparation command acquisition failed.");
        // All registry/cache changes finish on the owner before worker snapshots.
        // Their upload commands execute before the graph in the same submission.
        if(!PrepareMainPassResources(scene,*frame.preparation,draws) ||
            (drawShadows && !PrepareShadowPassResources(scene,draws)) ||
            (toneMap && !PrepareToneMap(renderFrame.color.texture)) ||
            (overlay && !PrepareCanvas(*overlay,*frame.preparation,overlayTarget,preparedCanvases[0])) ||
            (profilerOverlay && !PrepareCanvas(*profilerOverlay,*frame.preparation,overlayTarget,preparedCanvases[1])))
            return RendererFailure("Pass resource preparation failed.");
        if(!frame.preparation->Close())
            return RendererFailure("Pass preparation command recording failed.");
        if(RHI::ProfilerAccess::HasRecordedWork(*frame.preparation))
        {
            if(!nativeDevice->PrepareCommandLists(&frame.preparation,1,1))
                return RendererFailure("Pass preparation native recording failed.");
        }
        else
        {
            // Warm frames without uploads need no extra native command buffer.
            nativeDevice->DestroyCommandList(frame.preparation);
            frame.preparation=nullptr;
        }
        m_profiler.PrepareGpuScopes(drawShadows?2u:1u);
        // 그래프 Compile은 패스 의존성과 실행 순서를 구성하며 shader 컴파일이나 GPU 실행을 뜻하지 않는다.
        if(!graph.Compile()) return RendererFailure("Scene RenderGraph compilation failed."); // 그래프 의존성/자원 구성 검증 실패면 실행하지 않는다.
        if(!m_environmentCapture) // 환경 캡처를 제외한 창·텍스처 Scene 프레임에 패스별 GPU timestamp 묶음을 준비한다.
        {
            samples.packet.passes=graph.GetExecutionOrderNames();
            samples.packet.query=m_profiler.AcquireQuery(2+2*static_cast<uint32_t>(samples.packet.passes.size()));
            graph.SetTimestampQuery(samples.packet.query);
        }
        timing.cpuPrepareMilliseconds=prepareTimer.ElapsedMilliseconds()-timing.cpuSubmitMilliseconds;
        Platform::CpuTimer recordTimer;
        bool drawn;
        // ON은 독립 패스를 장치 작업자에, OFF는 단일 명령 목록에 기록하며 두 경로 모두 닫기·네이티브 변환까지 Record에 포함해 CPU 기록 시간의 비교 범위를 맞춘다.
        if(config.enableParallelRenderGraph)
            drawn = graph.ExecuteParallel(nativeDevice, frame.lists, config.recordingThreadCount);
        else
        {
            frame.lists.reserve(1);
            auto* commands = nativeDevice->AcquireCommandList();
            frame.lists.push_back(commands);
            drawn = commands && graph.Execute(commands) && commands->Close();
            drawn = drawn && nativeDevice->PrepareCommandLists(frame.lists.data(),1,1);
        }
        timing.cpuRecordMilliseconds=recordTimer.ElapsedMilliseconds();
        if(drawn && frame.preparation)
        {
            frame.lists.insert(frame.lists.begin(),frame.preparation);
            frame.preparation=nullptr;
        }
        Platform::CpuTimer submitTimer;
        RHI::FenceHandle completion;
        // 기록/네이티브 준비에 성공한 목록만 제출하고 완료 펜스를 받는다. Submit 성공은 수락된 작업을 뜻하며 펜스가 이미 완료되었음을 뜻하지 않는다.
        drawn = drawn && nativeDevice->Submit({frame.lists.data(),
            static_cast<uint32_t>(frame.lists.size()), nullptr, 0}, completion);
        timing.cpuSubmitMilliseconds+=submitTimer.ElapsedMilliseconds();
        if(drawn) { // draw 제출에 성공했을 때만 이력·통계·쿼리 소유권을 확정한다.
            if(frameNoAreaLights && recordingStatistics.mainDrawCount && !noAreaReported)
            {
                std::fputs("[Renderer] Main area-light-free shader active: uploaded Rect/Disc counts are zero; no graph lighting writes or global barriers.\n",stderr);
                noAreaReported=true;
            }
            if(frameOpaqueDepthComplete && !depthPrepassReported)
            {
                std::fputs("[Renderer] Main depth prepass active: early fragment tests, depth writes disabled, LessEqual.\n",stderr);
                depthPrepassReported=true;
            }
            m_profiler.SubmitGpuScopes(completion,liveFrame.GetKey());
            if(fixedOutput) outputPresentation->CommitFrame(); // 고정 타깃의 최종 자원 상태도 제출 성공 후 확정한다.
            canvasFrame.Commit();
            if(samples.packet.query && completion) { // 쿼리와 유효한 completion 펜스가 모두 있을 때만 비동기 GPU 표본을 수집기로 넘긴다. 넘긴 뒤 packet을 비워 지역 소멸자가 쿼리를 중복 반납하지 않게 한다.
                samples.packet.completion=completion;
                m_profiler.SubmitGpuSample(std::move(samples.packet));
                samples.packet={};
            }
            // draw 제출의 펜스와 기록한 통계를 보관한다. 뒤의 readback 또는 Present가 실패해도 이미 제출한 작업이 취소되지는 않으므로 이 값은 유지한다.
            lastRenderCompletion=completion;
            lastStatistics=recordingStatistics;
            if(materialIndexRecorded) {materialIndexReady=true;materialIndexBytes.swap(materialIndexScratch);} // 이번 draw 목록에 재질 인덱스 업로드가 있었으면 ready와 비교 기준 바이트를 갱신한다.
            // draw 제출이 성공한 시점에 아틀라스/모션/효과/GPU visibility 이력을 다음 프레임 기준으로 확정한다. 실제 GPU 접근 순서는 RHI 제출/배리어 계약에 따른다.
            CommitShadowAtlas(scene);
            CommitFrameResources(renderFrame);
            if(visibilityActive) gpuVisibility->CommitFrame(); // GPU 가시성 경로의 이번 프레임 이력도 제출 성공 기준으로 확정한다.
            frameResources.committed=true;
        }
        if(!drawn)return RendererFailure("Scene draw submission failed."); // 기록/네이티브 준비/제출 중 어느 단계든 실패했으면 호출 실패다.
        if(config.enableHdrRendering) hdrState=RHI::ResourceState::ShaderResource; // HDR을 쓴 경우 그래프의 최종 ShaderResource 상태를 저장한다.
        // 그래프가 약속한 최종 자원 상태를 추적 값으로 남겨 다음 프레임 import의 시작 상태로 사용한다.
        depthStencilState = sampledDepth ? RHI::ResourceState::ShaderResource : RHI::ResourceState::DepthWrite;
        shadowDepthState = shadowDepthTarget != nullptr
            ? RHI::ResourceState::ShaderResource
            : RHI::ResourceState::Undefined;
        Platform::CpuTimer readbackTimer;
        if(readback && !CaptureFrame(*readback))return false; // 캡처 요청이 실패하면 이미 제출한 펜스는 남기고 Render는 false를 반환한다.
        if(readback) timing.cpuReadbackMilliseconds=readbackTimer.ElapsedMilliseconds(); // 실제로 CPU 캡처를 요청한 경우만 readback 시간을 기록한다.
        if(!selectedOutput) // 창 경로만 Present를 수행한다.
        {
            Platform::CpuTimer presentTimer;
            const bool presented=nativeDevice->Present();
            timing.cpuPresentMilliseconds=presentTimer.ElapsedMilliseconds();
            framePending=false;
            if(!presented) return RendererFailure("Scene presentation failed."); // 표시 실패는 draw 제출 성공과 별개로 호출 실패를 반환한다.
        }
        if(!m_environmentCapture)
        {
            // 프레임 준비·기록·제출의 CPU 벽시계 시간 합이다. acquire/readback/Present 대기와 GPU 시간은 별도 항목이며 중첩 구간을 무조건 더하면 안 된다.
            timing.cpuRenderMilliseconds=timing.cpuPrepareMilliseconds+timing.cpuRecordMilliseconds+timing.cpuSubmitMilliseconds;
            RecordProfilerFrame(timing,scene.GetEntityCount());
            DY_PROFILE_FRAME_MARK();
        }
        shadowFrame.committed=true;
        // 이 Render 호출의 처리 성공을 게시한다. GPU 완료 증거는 별도의 completion 펜스와 완료된 timestamp 표본이다.
        liveFrame.SetOutcome(Platform::LiveOutcome::Completed);
        return true;
    }
    // 중간 예외는 범위 보호 객체를 거쳐 정리한 후 stderr와 false로 전파한다. 부분 실패를 렌더 성공으로 숨기지 않는다.
    catch(const std::exception& error)
    {
        std::fprintf(stderr,"dyf: Render: %s\n",error.what());
        return false;
    }
}

