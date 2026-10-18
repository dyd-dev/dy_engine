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
ControlValue Value(const RendererConfig& c,std::size_t i)
{
    switch(i){case 0:return c.enableInstancing;case 1:return c.enableFrustumCulling;case 2:return c.lighting.enabled;case 3:return c.lighting.shadows;case 4:return std::int64_t(c.lighting.shadowQuality);case 5:return c.enableHdrRendering;case 6:return double(c.exposure);case 7:return c.vsync;case 8:return c.enableParallelRenderGraph;
    case 10:return c.enableIncrementalTransformUploads;case 11:return c.enableResourceSetCache;case 12:return c.enableBindlessMaterials;case 13:return c.enableCanvasBatching;case 14:return c.enableCanvasImageCache;case 15:return c.enableCanvasVertexBufferReuse;default:
#ifdef DY_SIMD_ENABLED
        return true;
#else
        return false;
#endif
    }
}
void SetValue(RendererConfig& c,std::size_t i,const ControlValue& v)
{
    switch(i){case 0:c.enableInstancing=std::get<bool>(v);break;case 1:c.enableFrustumCulling=std::get<bool>(v);break;case 2:c.lighting.enabled=std::get<bool>(v);break;case 3:c.lighting.shadows=std::get<bool>(v);break;case 4:c.lighting.shadowQuality=static_cast<ShadowQuality>(std::get<std::int64_t>(v));break;case 5:c.enableHdrRendering=std::get<bool>(v);break;case 6:c.exposure=float(std::get<double>(v));break;case 7:c.vsync=std::get<bool>(v);break;case 8:c.enableParallelRenderGraph=std::get<bool>(v);break;
    case 10:c.enableIncrementalTransformUploads=std::get<bool>(v);break;case 11:c.enableResourceSetCache=std::get<bool>(v);break;case 12:c.enableBindlessMaterials=std::get<bool>(v);break;case 13:c.enableCanvasBatching=std::get<bool>(v);break;case 14:c.enableCanvasImageCache=std::get<bool>(v);break;case 15:c.enableCanvasVertexBufferReuse=std::get<bool>(v);break;default:break;}
}
bool SameConfig(const RendererConfig& a,const RendererConfig& b)
{
    if(a.vsync!=b.vsync||a.allowReadback!=b.allowReadback||a.enableHdrRendering!=b.enableHdrRendering||a.enableProfilerHud!=b.enableProfilerHud||a.profilerStartsExpanded!=b.profilerStartsExpanded||a.enableParallelRenderGraph!=b.enableParallelRenderGraph||a.recordingThreadCount!=b.recordingThreadCount||a.enableInstancing!=b.enableInstancing||a.enableFrustumCulling!=b.enableFrustumCulling||a.lighting.enabled!=b.lighting.enabled||a.lighting.shadows!=b.lighting.shadows||a.lighting.shadowQuality!=b.lighting.shadowQuality||a.enableIncrementalTransformUploads!=b.enableIncrementalTransformUploads||a.enableResourceSetCache!=b.enableResourceSetCache||a.enableBindlessMaterials!=b.enableBindlessMaterials||a.enableCanvasBatching!=b.enableCanvasBatching||a.enableCanvasImageCache!=b.enableCanvasImageCache||a.enableCanvasVertexBufferReuse!=b.enableCanvasVertexBufferReuse)return false;
    const float av[]={a.clearColor.x,a.clearColor.y,a.clearColor.z,a.clearColor.w,a.exposure,a.lighting.ambientColor.x,a.lighting.ambientColor.y,a.lighting.ambientColor.z,a.lighting.ambientIntensity,a.lighting.environment.diffuseColor.x,a.lighting.environment.diffuseColor.y,a.lighting.environment.diffuseColor.z,a.lighting.environment.diffuseIntensity,a.lighting.environment.specularColor.x,a.lighting.environment.specularColor.y,a.lighting.environment.specularColor.z,a.lighting.environment.specularIntensity};
    const float bv[]={b.clearColor.x,b.clearColor.y,b.clearColor.z,b.clearColor.w,b.exposure,b.lighting.ambientColor.x,b.lighting.ambientColor.y,b.lighting.ambientColor.z,b.lighting.ambientIntensity,b.lighting.environment.diffuseColor.x,b.lighting.environment.diffuseColor.y,b.lighting.environment.diffuseColor.z,b.lighting.environment.diffuseIntensity,b.lighting.environment.specularColor.x,b.lighting.environment.specularColor.y,b.lighting.environment.specularColor.z,b.lighting.environment.specularIntensity};
    return std::memcmp(av,bv,sizeof(av))==0;
}
}
void Renderer::Impl::InitializeLiveProfiling() noexcept
{
    if(liveId||!IsLiveCollectorEnabled())return;
    try{
        const auto deviceId=RegisterLiveObject(device,{LiveObjectKind::Device,0,0,0,0,"RHI device"});
        liveId=RegisterLiveObject(&owner,{LiveObjectKind::Renderer,deviceId,0,0,0,"Renderer"});liveAppliedConfig=config;
        const char* keys[]={"instancing","culling","lighting","shadows","shadowQuality","hdr","exposure","vsync","parallelRecording","simd","optimization.transformUploads","optimization.resourceSetCache","optimization.bindlessMaterials","canvas.batching","canvas.imageCache","canvas.vertexBufferReuse"};
        const char* descriptions[]={"Group compatible stock Scene instances","Cull stock Scene entities against the CPU frustum","Apply lighting to material output","Render supported light shadow maps","Select shadow resolution and filtering","Render through the HDR intermediate","Select tone-map exposure","Select presentation synchronization","Record independent render-graph passes on the device-owned workers","Use the compiled SIMD math path","Upload only changed stock Scene instance transforms","Reuse equal Renderer resource sets","Use the supported stock bindless material path","Merge adjacent compatible Canvas draws","Reuse immutable Canvas images between frames","Reuse Canvas vertex buffer capacity"};
        const char* onPaths[]={"Compatible mesh/material instances share a draw","Visible stock entities feed main batches","Light and environment constants affect shading","Shadow passes and receivers are enabled","Selected quality controls shadow targets","HDR color target and tone-map pass","Selected exposure constant","FIFO presentation","Independent CPU recording jobs use the ThreadPool","Compiled SIMD implementation where called","Unchanged transforms skip upload; adjacent changes form ranges","Match pipeline and complete resource bindings","Bindless stock shader and material layout when device-supported","Adjacent equal texture/viewport/scissor ranges share one draw","Immutable pixel identity, dimensions and color space reuse texture/set","Keep completed capacity and upload the current vertices"};
        const char* offPaths[]={"One draw per stock entity","All stock entities feed main batches","Unlit base/emissive material output","No shadow passes","Choose another quality; not an equivalent algorithm bypass","Direct non-HDR output","Choose another exposure; output changes","Immediate presentation when supported","Record the same graph serially","Requires another build; no runtime bypass","Upload every instance transform on every frame","Create each resource set; preserve deferred retirement","Existing non-bindless stock shader and material layout","Issue original adjacent draws in source order","Recreate each unique image per frame; deduplicate within that frame","Allocate a fresh vertex buffer per frame"};
        for(std::size_t i=0;i<liveControls.size();++i){ControlDesc d;d.device=deviceId;d.source=liveId;d.owner=liveId;d.key=keys[i];d.label=keys[i];d.appliedValue=Value(config,i);
            d.category=(i==2||i==3||i==4||i==5||i==6||i==7)?ControlCategory::OutputQuality:ControlCategory::Optimization;d.description=descriptions[i];d.onPath=onPaths[i];d.offPath=offPaths[i];d.conditions="Only frames that execute this path contribute measured work; overlapping CPU scopes are not additive";d.tradeoffs="Compare the original stage costs, uploads, allocations and commands with the same workload";
            if(i==0||i==3||i==4||i==5||i==7||i==12)d.application=ControlApplication::ResourceRebuild;
            if(i==4){d.kind=ControlKind::Enumeration;d.choices={{0,"Low"},{1,"Medium"},{2,"High"}};}
            if(i==6){d.kind=ControlKind::Real;d.minimum=0;d.maximum=FLT_MAX;}
            if(i==9){d.writable=false;d.category=ControlCategory::BuildOrStructural;d.application=ControlApplication::RebuildRequired;d.unavailableReason="SIMD is selected at compile time";d.conditions="Math operations that dispatch to SIMD; the batch helper is not assumed to run in Renderer";d.tradeoffs="Requires equivalent builds and inputs for a separate experiment";}
            if(i==0||i==1||i==10)d.conditions="Stock Scene geometry; incremental transforms additionally require instance streaming; Model/custom draw inputs bypass these paths";
            if(i==8){d.conditions="Device-owned recording workers and independent render-graph passes; CPU recording only";d.tradeoffs="Scheduling and synchronization cost can exceed saved recording time; this does not imply GPU parallelism";}
            if(i==12){d.conditions="Stock fragment shader and device support for the actual bindless layout; unsupported/custom shaders use the non-bindless path in either setting";d.tradeoffs="Rebuilds the stock pipeline; material/index uploads and binding costs depend on the workload";}
            if(i>=13)d.conditions="Frames that render Canvas, including overlays; image identity includes color space; alpha and source order are preserved";
            liveControls[i]=RegisterControl(std::move(d),[this,i](const ControlRequest& r,LiveFrameKey){auto next=GetConfig();SetValue(next,i,r.value);
                if(!SetConfig(next))return ControlApplyResult{ControlState::Failed,Value(config,i),"Renderer rejected configuration"};liveRequests[i]=r.request;
                return ControlApplyResult{ControlState::PendingApply,Value(config,i),{},Value(next,i)};
            });
        }
        const char* catalogKeys[]={"scene.componentArrays","renderer.staticGeometryCache","renderer.textureCache","renderer.constantUploadElision","renderer.shadowBoundsReuse"};
        const char* catalogDescriptions[]={"Scene stores components in separate arrays","Cache prepared immutable mesh geometry","Cache immutable material textures","Skip equal lighting/shadow constant bytes","Reuse per-frame shadow light/caster bounds"};
        const char* catalogPaths[]={"Component arrays feed existing Data/CPU preparation stages","Prepared scene mesh identity retains vertex/index buffers","Pixel identity and color space retain uploaded textures","Compare committed constants before issuing a logical upload","Compute shared bounds before recording related shadow passes"};
        const char* catalogConditions[]={"Structural component vectors; not a full coordinate SoA or a measured AoS comparison","Scene mesh preparation when geometry is unchanged","Material texture resolution when immutable images are reused","Lighting/shadow constants; attempted, failed and committed state remain distinct","Frames with supported lights and shadow passes"};
        for(std::size_t i=0;i<liveCatalog.size();++i){ControlDesc d;d.device=deviceId;d.source=d.owner=liveId;d.key=catalogKeys[i];d.label=catalogDescriptions[i];d.description=catalogDescriptions[i];d.onPath=catalogPaths[i];d.offPath="No equivalent runtime reference path is implemented";d.conditions=catalogConditions[i];d.tradeoffs="Observation only; use a separate equivalent experiment to attribute this optimization";d.category=i==0?ControlCategory::BuildOrStructural:ControlCategory::Optimization;d.appliedValue=true;d.writable=false;d.application=i==0?ControlApplication::RebuildRequired:ControlApplication::Immediate;d.unavailableReason=d.offPath;liveCatalog[i]=RegisterControl(std::move(d),{});}
    }catch(...){}
}
bool Renderer::Impl::PrepareLiveFrame(LiveFrameKey frame) noexcept
{
    if(frame.frame&&livePreparedFrame==frame.frame){
        if(!shadersPending)return true;
        // Model configures its vertex/compute shaders after the outer settings boundary.
        auto deferred=std::exchange(pendingConfig,nullptr);
        try{const bool applied=ApplySettings();pendingConfig=std::move(deferred);return applied;}catch(...){pendingConfig=std::move(deferred);return false;}
    }
    try{m_profiler.CollectGpuScopes();PollControls(liveId,frame);const bool applied=ApplySettings();FinishLiveSettings(applied,frame);if(applied)livePreparedFrame=frame.frame;return applied;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: Renderer settings: %s\n",error.what());FinishLiveSettings(false,frame);return false;}
    catch(...){std::fprintf(stderr,"dyf: Renderer settings failed.\n");FinishLiveSettings(false,frame);return false;}
}
void Renderer::Impl::FinishLiveSettings(bool success,LiveFrameKey frame) noexcept
{
    if(!liveId)return;
    if(!success){for(auto& request:liveRequests){if(request)FailControl(request,"Renderer apply failed; existing pending settings retained");request=0;}return;}
    NotifyLiveSettingsApplied(liveId,frame,!SameConfig(liveAppliedConfig,config));liveAppliedConfig=config;
    for(std::size_t i=0;i<liveControls.size();++i){const auto value=Value(config,i);if(liveRequests[i]){CompleteControl(liveRequests[i],frame,value);liveRequests[i]=0;}PublishControlValue(liveControls[i].GetId(),value);}
    PublishControlAvailability(liveControls[8].GetId(),true,"");
    PublishLiveGeometryAvailability(liveCustomDraws);
}
void Renderer::Impl::PublishLiveGeometryAvailability(bool customDraws) noexcept
{
    liveCustomDraws=customDraws;
    const auto reason=customDraws?"Stock Scene geometry only; Model/custom draw inputs bypass instancing and CPU frustum culling":"";
    PublishControlAvailability(liveControls[0].GetId(),!customDraws,reason);
    PublishControlAvailability(liveControls[1].GetId(),!customDraws,reason);
    const bool transforms=!customDraws&&UsesInstanceStream();
    PublishControlAvailability(liveControls[10].GetId(),transforms,transforms?"":"Incremental transform uploads require stock Scene instance streaming; Model/custom geometry bypasses this path");
    try{const bool bindless=SupportsBindlessMaterials();PublishControlAvailability(liveControls[12].GetId(),bindless,bindless?"":"Bindless materials require the stock fragment shader and device support for the actual bindless layout");}catch(...){PublishControlAvailability(liveControls[12].GetId(),false,"Bindless layout availability check failed");}
}
}
