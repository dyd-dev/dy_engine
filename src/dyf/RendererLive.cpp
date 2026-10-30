#include "../Platform/Profiler/CollectorInternal.h"
#include "RendererInternal.h"
#include "dyf/RHI/ICommandList.h"
#include "../Platform/Profiler/GpuTimestamp.h"
#include <cfloat>
#include <cstring>
#include <cstdio>
#include <utility>

namespace dyf
{
using namespace Platform;
namespace
{
// 고정된 제어 인덱스를 bool/int64/double 값으로 읽는다. 9번 SIMD는 런타임 설정이 아니라 현재 빌드의 컴파일 옵션을 보고한다.
ControlValue Value(const RendererConfig& c,std::size_t i)
{
    switch(i){case 0:return c.enableInstancing;case 1:return c.enableFrustumCulling;case 2:return c.lighting.enabled;case 3:return c.lighting.shadows;case 4:return std::int64_t(c.lighting.shadowQuality);case 5:return c.enableHdrRendering;case 6:return double(c.exposure);case 7:return c.vsync;case 8:return c.enableParallelRenderGraph;
    case 10:return c.enableIncrementalTransformUploads;case 11:return c.enableResourceSetCache;case 12:return c.enableBindlessMaterials;case 13:return c.enableCanvasBatching;case 14:return c.enableCanvasImageCache;case 15:return c.enableCanvasVertexBufferReuse;default:
// SIMD 포함 빌드인지 true/false로 보고하며 실행 중 SIMD 설정을 바꾸지는 않는다.
#ifdef DY_SIMD_ENABLED
        return true;
#else
        return false;
#endif
    }
}
// 수정 가능한 인덱스만 요청 variant에서 해당 형식으로 꺼내 RendererConfig에 반영한다. SIMD를 포함한 나머지 인덱스는 바꾸지 않는다.
void SetValue(RendererConfig& c,std::size_t i,const ControlValue& v)
{
    switch(i){case 0:c.enableInstancing=std::get<bool>(v);break;case 1:c.enableFrustumCulling=std::get<bool>(v);break;case 2:c.lighting.enabled=std::get<bool>(v);break;case 3:c.lighting.shadows=std::get<bool>(v);break;case 4:c.lighting.shadowQuality=static_cast<ShadowQuality>(std::get<std::int64_t>(v));break;case 5:c.enableHdrRendering=std::get<bool>(v);break;case 6:c.exposure=float(std::get<double>(v));break;case 7:c.vsync=std::get<bool>(v);break;case 8:c.enableParallelRenderGraph=std::get<bool>(v);break;
    case 10:c.enableIncrementalTransformUploads=std::get<bool>(v);break;case 11:c.enableResourceSetCache=std::get<bool>(v);break;case 12:c.enableBindlessMaterials=std::get<bool>(v);break;case 13:c.enableCanvasBatching=std::get<bool>(v);break;case 14:c.enableCanvasImageCache=std::get<bool>(v);break;case 15:c.enableCanvasVertexBufferReuse=std::get<bool>(v);break;default:break;}
}
// 라이브 설정 revision의 변화 판정에 쓰이는 비교다. 여기 나열한 bool/열거/스칼라만 비교하므로 RendererConfig 모든 필드의 완전 동등 비교로 확대해 읽지 않는다.
bool SameConfig(const RendererConfig& a,const RendererConfig& b)
{
    // 아래 정수·열거·플래그 중 하나라도 다르면 라이브 설정 변경으로 판단한다.
    if(a.vsync!=b.vsync||a.allowReadback!=b.allowReadback||a.enableHdrRendering!=b.enableHdrRendering||a.enableProfilerHud!=b.enableProfilerHud||a.profilerStartsExpanded!=b.profilerStartsExpanded||a.enableParallelRenderGraph!=b.enableParallelRenderGraph||a.recordingThreadCount!=b.recordingThreadCount||a.enableInstancing!=b.enableInstancing||a.enableFrustumCulling!=b.enableFrustumCulling||a.lighting.enabled!=b.lighting.enabled||a.lighting.shadows!=b.lighting.shadows||a.lighting.shadowQuality!=b.lighting.shadowQuality||a.enableIncrementalTransformUploads!=b.enableIncrementalTransformUploads||a.enableResourceSetCache!=b.enableResourceSetCache||a.enableBindlessMaterials!=b.enableBindlessMaterials||a.enableCanvasBatching!=b.enableCanvasBatching||a.enableCanvasImageCache!=b.enableCanvasImageCache||a.enableCanvasVertexBufferReuse!=b.enableCanvasVertexBufferReuse)return false;
    // 두 설정의 대응 float 묶음을 같은 순서로 만들어 비트 단위 비교한다. 값의 근사 동등성이 아니라 저장 표현 일치 여부를 사용한다.
    const float av[]={a.clearColor.x,a.clearColor.y,a.clearColor.z,a.clearColor.w,a.exposure,a.lighting.ambientColor.x,a.lighting.ambientColor.y,a.lighting.ambientColor.z,a.lighting.ambientIntensity,a.lighting.environment.diffuseColor.x,a.lighting.environment.diffuseColor.y,a.lighting.environment.diffuseColor.z,a.lighting.environment.diffuseIntensity,a.lighting.environment.specularColor.x,a.lighting.environment.specularColor.y,a.lighting.environment.specularColor.z,a.lighting.environment.specularIntensity};
    const float bv[]={b.clearColor.x,b.clearColor.y,b.clearColor.z,b.clearColor.w,b.exposure,b.lighting.ambientColor.x,b.lighting.ambientColor.y,b.lighting.ambientColor.z,b.lighting.ambientIntensity,b.lighting.environment.diffuseColor.x,b.lighting.environment.diffuseColor.y,b.lighting.environment.diffuseColor.z,b.lighting.environment.diffuseIntensity,b.lighting.environment.specularColor.x,b.lighting.environment.specularColor.y,b.lighting.environment.specularColor.z,b.lighting.environment.specularIntensity};
    return std::memcmp(av,bv,sizeof(av))==0;
}
}
// collector가 켜졌을 때 장치→Renderer 계층과 16개 제어를 한 번 등록한다. 프로파일링 등록 실패는 noexcept 경계에서 삼켜 렌더 기능의 예외로 번지지 않게 한다.
void Renderer::Impl::InitializeLiveProfiling() noexcept
{
    if(liveId||!IsLiveCollectorEnabled())return; // 이미 등록했거나 수집기가 꺼져 있으면 등록을 생략한다.
    try{
        const auto deviceId=RegisterLiveObject(device,{LiveObjectKind::Device,0,0,0,0,"RHI device"});
        liveId=RegisterLiveObject(&owner,{LiveObjectKind::Renderer,deviceId,0,0,0,"Renderer"});liveAppliedConfig=config;
        // 배열 인덱스가 Value/SetValue와 같은 의미를 가져야 한다. 각 제어의 키·설명·ON/OFF 경로를 병렬 배열로 구성한다.
        const char* keys[]={"instancing","culling","lighting","shadows","shadowQuality","hdr","exposure","vsync","parallelRecording","simd","optimization.transformUploads","optimization.resourceSetCache","optimization.bindlessMaterials","canvas.batching","canvas.imageCache","canvas.vertexBufferReuse"};
        const char* descriptions[]={"Group compatible stock Scene instances","Cull stock Scene entities against the CPU frustum","Apply lighting to material output","Render supported light shadow maps","Select shadow resolution and filtering","Render through the HDR intermediate","Select tone-map exposure","Select presentation synchronization","Record independent render-graph passes on the device-owned workers","Use the compiled SIMD math path","Upload only changed stock Scene instance transforms","Reuse equal Renderer resource sets","Use the supported stock bindless material path","Merge adjacent compatible Canvas draws","Reuse immutable Canvas images between frames","Reuse Canvas vertex buffer capacity"};
        const char* onPaths[]={"Compatible mesh/material instances share a draw","Visible stock entities feed main batches","Light and environment constants affect shading","Shadow passes and receivers are enabled","Selected quality controls shadow targets","HDR color target and tone-map pass","Selected exposure constant","FIFO presentation","Independent CPU recording jobs use the ThreadPool","Compiled SIMD implementation where called","Unchanged transforms skip upload; adjacent changes form ranges","Match pipeline and complete resource bindings","Bindless stock shader and material layout when device-supported","Adjacent equal texture/viewport/scissor ranges share one draw","Immutable pixel identity, dimensions and color space reuse texture/set","Keep completed capacity and upload the current vertices"};
        const char* offPaths[]={"One draw per stock entity","All stock entities feed main batches","Unlit base/emissive material output","No shadow passes","Choose another quality; not an equivalent algorithm bypass","Direct non-HDR output","Choose another exposure; output changes","Immediate presentation when supported","Record the same graph serially","Requires another build; no runtime bypass","Upload every instance transform on every frame","Create each resource set; preserve deferred retirement","Existing non-bindless stock shader and material layout","Issue original adjacent draws in source order","Recreate each unique image per frame; deduplicate within that frame","Allocate a fresh vertex buffer per frame"};
        // 각 제어에 device/source/owner를 연결하고 현재 적용값을 초기값으로 게시한다. 출력 품질 변화와 최적화 토글을 구분해 비교의 의미를 표시한다.
        for(std::size_t i=0;i<liveControls.size();++i){ControlDesc d;d.device=deviceId;d.source=liveId;d.owner=liveId;d.key=keys[i];d.label=keys[i];d.appliedValue=Value(config,i);
            d.category=(i==2||i==3||i==4||i==5||i==6||i==7)?ControlCategory::OutputQuality:ControlCategory::Optimization;d.description=descriptions[i];d.onPath=onPaths[i];d.offPath=offPaths[i];d.conditions="Only frames that execute this path contribute measured work; overlapping CPU scopes are not additive";d.tradeoffs="Compare the original stage costs, uploads, allocations and commands with the same workload";
            if(i==0||i==3||i==4||i==5||i==7||i==12)d.application=ControlApplication::ResourceRebuild; // 해당 제어는 파이프라인/출력 자원 재구성이 필요한 유형으로 표시한다.
            if(i==4){d.kind=ControlKind::Enumeration;d.choices={{0,"Low"},{1,"Medium"},{2,"High"}};} // 그림자 품질은 Low/Medium/High 열거 선택지로 게시한다.
            if(i==6){d.kind=ControlKind::Real;d.minimum=0;d.maximum=FLT_MAX;} // 노출은 0 이상 float 최대값 범위의 실수 입력으로 게시한다.
            // SIMD는 빌드 선택이므로 읽기 전용·재빌드 필요로 표시한다.
            if(i==9){d.writable=false;d.category=ControlCategory::BuildOrStructural;d.application=ControlApplication::RebuildRequired;d.unavailableReason="SIMD is selected at compile time";d.conditions="Math operations that dispatch to SIMD; the batch helper is not assumed to run in Renderer";d.tradeoffs="Requires equivalent builds and inputs for a separate experiment";}
            // 기본 Scene 입력·인스턴스 스트림 조건과 Model·사용자 입력의 우회 경로를 안내한다.
            if(i==0||i==1||i==10)d.conditions="Stock Scene geometry; incremental transforms additionally require instance streaming; Model/custom draw inputs bypass these paths";
            // 병렬 기록은 CPU 작업자·독립 패스에 관한 제어임을 명시한다.
            if(i==8){d.conditions="Device-owned recording workers and independent render-graph passes; CPU recording only";d.tradeoffs="Scheduling and synchronization cost can exceed saved recording time; this does not imply GPU parallelism";}
            // 표준 fragment와 실제 bindless 레이아웃 지원이 있어야 의미 있는 토글이다.
            if(i==12){d.conditions="Stock fragment shader and device support for the actual bindless layout; unsupported/custom shaders use the non-bindless path in either setting";d.tradeoffs="Rebuilds the stock pipeline; material/index uploads and binding costs depend on the workload";}
            if(i>=13)d.conditions="Frames that render Canvas, including overlays; image identity includes color space; alpha and source order are preserved"; // Canvas 계열 제어는 Canvas/overlay를 렌더하는 프레임에서만 작업을 바꾼다.
            // 요청 콜백은 새 config를 예약하고 PendingApply를 돌려준다. 기존 적용값과 요청값을 따로 전달하며 ApplySettings 성공 전에는 완료 처리하지 않는다.
            liveControls[i]=RegisterControl(std::move(d),[this,i](const ControlRequest& r,LiveFrameKey){auto next=GetConfig();SetValue(next,i,r.value);
                if(!SetConfig(next))return ControlApplyResult{ControlState::Failed,Value(config,i),"Renderer rejected configuration"};liveRequests[i]=r.request; // 요청 설정 검증 실패면 Failed, 성공하면 요청 ID를 보관해 다음 적용 완료를 기다린다.
                return ControlApplyResult{ControlState::PendingApply,Value(config,i),{},Value(next,i)};
            });
        }
        // 실제 ON/OFF 대조 경로가 없는 구조/캐시 특성은 관측용 카탈로그로만 게시한다. writable=false이므로 실시간 성능 대조 토글로 사용할 수 없다.
        const char* catalogKeys[]={"scene.componentArrays","renderer.staticGeometryCache","renderer.textureCache","renderer.constantUploadElision","renderer.shadowBoundsReuse"};
        const char* catalogDescriptions[]={"Scene stores components in separate arrays","Cache prepared immutable mesh geometry","Cache immutable material textures","Skip equal lighting/shadow constant bytes","Reuse per-frame shadow light/caster bounds"};
        const char* catalogPaths[]={"Component arrays feed existing Data/CPU preparation stages","Prepared scene mesh identity retains vertex/index buffers","Pixel identity and color space retain uploaded textures","Compare committed constants before issuing a logical upload","Compute shared bounds before recording related shadow passes"};
        const char* catalogConditions[]={"Structural component vectors; not a full coordinate SoA or a measured AoS comparison","Scene mesh preparation when geometry is unchanged","Material texture resolution when immutable images are reused","Lighting/shadow constants; attempted, failed and committed state remain distinct","Frames with supported lights and shadow passes"};
        // 구조적 특성과 기존 최적화의 조건을 설명용 제어로 등록한다. true는 경로 존재 설명이며 성능 개선량이나 GPU 작업 완료의 측정값은 아니다.
        for(std::size_t i=0;i<liveCatalog.size();++i){ControlDesc d;d.device=deviceId;d.source=d.owner=liveId;d.key=catalogKeys[i];d.label=catalogDescriptions[i];d.description=catalogDescriptions[i];d.onPath=catalogPaths[i];d.offPath="No equivalent runtime reference path is implemented";d.conditions=catalogConditions[i];d.tradeoffs="Observation only; use a separate equivalent experiment to attribute this optimization";d.category=i==0?ControlCategory::BuildOrStructural:ControlCategory::Optimization;d.appliedValue=true;d.writable=false;d.application=i==0?ControlApplication::RebuildRequired:ControlApplication::Immediate;d.unavailableReason=d.offPath;liveCatalog[i]=RegisterControl(std::move(d),{});}
    }catch(...){}
}
// 프레임당 제어 polling/설정 적용을 한 번 수행한다. 이미 준비한 같은 프레임에 Model이 셰이더를 바꾼 경우에는 일반 config 예약을 잠시 보관하고 셰이더만 추가 적용한다.
bool Renderer::Impl::PrepareLiveFrame(LiveFrameKey frame) noexcept
{
    if(frame.frame&&livePreparedFrame==frame.frame){ // 유효한 같은 프레임을 이미 준비했다면 중복 제어 적용 경로를 피한다.
        if(!shadersPending)return true; // 같은 프레임의 추가 셰이더 요청도 없으면 즉시 준비 성공이다.
        // Model configures its vertex/compute shaders after the outer settings boundary.
        // 기존 config 요청의 소유권을 지역 변수로 옮겨 이번 중첩 셰이더 적용에 섞이지 않게 한다. 성공/예외 두 경로 모두 다시 pendingConfig로 돌려놓는다.
        auto deferred=std::exchange(pendingConfig,nullptr);
        try{const bool applied=ApplySettings();pendingConfig=std::move(deferred);return applied;}catch(...){pendingConfig=std::move(deferred);return false;}
    }
    // 완료된 GPU scope 수집 → 제어 조회 → ApplySettings → 결과 게시 후 성공한 프레임만 기억하며, 예외는 stderr·Failed·false로 전달해 다음 경계의 재시도를 허용한다.
    try{m_profiler.CollectGpuScopes();PollControls(liveId,frame);const bool applied=ApplySettings();FinishLiveSettings(applied,frame);if(applied)livePreparedFrame=frame.frame;return applied;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Renderer settings: %s\n",error.what());FinishLiveSettings(false,frame);return false;}
    catch(...){std::fprintf(stderr,"dyf: Renderer settings failed.\n");FinishLiveSettings(false,frame);return false;}
}
// 적용 실패 요청은 Failed로 끝내되 내부 pending 설정은 그대로 남을 수 있다. 적용 성공은 실제 frame과 실제 config 값을 묶어 요청 완료와 현재 값을 게시한다.
void Renderer::Impl::FinishLiveSettings(bool success,LiveFrameKey frame) noexcept
{
    if(!liveId)return; // 라이브 객체를 등록하지 않았다면 제어 결과를 게시할 대상이 없다.
    if(!success){for(auto& request:liveRequests){if(request)FailControl(request,"Renderer apply failed; existing pending settings retained");request=0;}return;} // 적용 실패면 보관한 요청을 실패 처리하고 요청 ID를 비운다. 내부 예약 설정까지 지우지는 않는다.
    // 직전 적용 스냅샷과 비교해 변경 여부를 알리고 현재 config를 보관한다. 이 비교 범위는 위 SameConfig에 나열된 항목이다.
    try
    {
        auto snapshot=config;
        const bool changed=!SameConfig(liveAppliedConfig,config);
        liveAppliedConfig=std::move(snapshot);
        NotifyLiveSettingsApplied(liveId,frame,changed);
    }
    catch(...) {std::fputs("dyf: Renderer live settings snapshot failed.\n",stderr);}
    // 대기 요청을 실제 적용 프레임·값으로 완료하고 ID를 비운 뒤, 요청 유무와 관계없이 모든 제어의 현재 적용값을 게시한다.
    for(std::size_t i=0;i<liveControls.size();++i){const auto value=Value(config,i);if(liveRequests[i]){CompleteControl(liveRequests[i],frame,value);liveRequests[i]=0;}PublishControlValue(liveControls[i].GetId(),value);}
    PublishControlAvailability(liveControls[8].GetId(),true,"");
    PublishLiveGeometryAvailability(liveCustomDraws);
}
// 현재 입력과 장치의 조건에 맞춰 제어 UI의 사용 가능 여부를 갱신한다. 요청 값 자체를 바꾸거나 지원되지 않는 효과를 실행했다고 보고하지 않는다.
void Renderer::Impl::PublishLiveGeometryAvailability(bool customDraws) noexcept
{
    liveCustomDraws=customDraws;
    // main의 RenderScene 진입부는 draws 포인터 유무로 이 플래그를 전달한다. 뒤에서 계산하는 stockDrawInputs의 상세 호환 판정과 구분해서 읽는다.
    const auto reason=customDraws?"Stock Scene geometry only; Model/custom draw inputs bypass instancing and CPU frustum culling":"";
    PublishControlAvailability(liveControls[0].GetId(),!customDraws,reason);
    PublishControlAvailability(liveControls[1].GetId(),!customDraws,reason);
    const bool transforms=!customDraws&&UsesInstanceStream();
    PublishControlAvailability(liveControls[10].GetId(),transforms,transforms?"":"Incremental transform uploads require stock Scene instance streaming; Model/custom geometry bypasses this path");
    // 지원 조회 성공 시 결과를 게시하고, 조회 예외면 사용 불가와 이유를 게시한다.
    try{const bool bindless=SupportsBindlessMaterials();PublishControlAvailability(liveControls[12].GetId(),bindless,bindless?"":"Bindless materials require the stock fragment shader and device support for the actual bindless layout");}catch(...){PublishControlAvailability(liveControls[12].GetId(),false,"Bindless layout availability check failed");}
}
}
