#pragma once

#include "dyf/Platform/Input.h"
#include <cstddef>
#include <iterator>
#include <string_view>

namespace dy_examples::controls
{
struct Choice { const char* value; const char* label; };
enum class ArgumentKind { Choice, Integer, Number, Flag, Path };

// Arguments are parsed by the original entry point at startup/reload. They are
// not live setters. Omit disabled optional controls and empty values. Positional
// controls have no flag. A flag ending in '=' joins its value into one argv item.
// arity > 1 emits separate value items, not one space-containing argv item.
struct ArgumentControl
{
    const char* label;
    const char* flag;
    const char* defaultValue;
    ArgumentKind kind;
    double minimum=0, maximum=0;
    const Choice* choices=nullptr;
    std::size_t choiceCount=0;
    bool optional=false;
    bool positional=false;
    unsigned arity=1;
};
struct ActionControl
{
    const char* label;
    dyf::Platform::Key key;
    bool hold=false;
};
struct ModuleControls
{
    const char* target;
    const ArgumentControl* args;
    std::size_t argCount;
    const ActionControl* actions;
    std::size_t actionCount;
    const char* notes;
};

namespace detail
{
using Key=dyf::Platform::Key;
constexpr double U32Max=4294967295.0;

template<std::size_t N>
constexpr ArgumentControl Select(const char* label,const char* flag,const char* value,const Choice (&choices)[N],
    bool optional=false,bool positional=false,unsigned arity=1)
{ return {label,flag,value,ArgumentKind::Choice,0,0,choices,N,optional,positional,arity}; }
constexpr ArgumentControl Integer(const char* label,const char* flag,const char* value,double minimum,double maximum,
    bool optional=false,unsigned arity=1)
{ return {label,flag,value,ArgumentKind::Integer,minimum,maximum,nullptr,0,optional,false,arity}; }
constexpr ArgumentControl Number(const char* label,const char* flag,const char* value,double minimum,double maximum,
    bool optional=false,unsigned arity=1)
{ return {label,flag,value,ArgumentKind::Number,minimum,maximum,nullptr,0,optional,false,arity}; }
constexpr ArgumentControl Flag(const char* label,const char* flag,const char* value="0")
{ return {label,flag,value,ArgumentKind::Flag,0,1,nullptr,0,false,false,0}; }
constexpr ArgumentControl Path(const char* label,const char* flag,const char* value="",bool positional=false)
{ return {label,flag,value,ArgumentKind::Path,0,0,nullptr,0,true,positional,1}; }
constexpr auto Frames=Integer("Frame limit (0 = unlimited)","--frames","0",0,U32Max);
constexpr auto Validation=Flag("RHI validation","--validation");
constexpr auto FixedTime=Number("Fixed time (seconds)","--time","0",0,1000000,true);
constexpr auto Capture=Path("Output capture (PPM)","--capture");
constexpr auto Exposure=Number("Exposure","--exposure","1",.01,20);
constexpr auto Roughness=Number("Roughness","--roughness","0.45",.045,1);
constexpr auto Metallic=Number("Metallic","--metallic","0.8",0,1);

inline constexpr Choice DepthModes[]={{"split","Split raw / linear"},{"raw","Raw depth"},{"linear","Linear depth"}};
inline constexpr Choice StencilModes[]={{"mask","Stencil mask"},{"outline","Outline"}};
inline constexpr Choice PresentModes[]={{"fifo","FIFO"},{"immediate","Immediate"},{"mailbox","Mailbox"}};
inline constexpr Choice PbrModes[]={{"pbr","PBR"},{"lambert","Lambert"},{"normal","Normals"}};
inline constexpr Choice ShadowModes[]={{"shadow","Shadowed"},{"unshadowed","Unshadowed"},{"depth","Shadow depth"},
    {"visibility","Visibility"},{"regions","Cascade regions"}};
inline constexpr Choice ShadowTypes[]={{"directional","Directional"},{"cascaded","Cascaded"},{"point","Point"}};
inline constexpr Choice ClusterModes[]={{"clustered","Clustered"},{"all","All lights"},{"count","Light count"}};
inline constexpr Choice DeferredModes[]={{"lit","Lit"},{"normal","Normals"},{"albedo","Albedo"},{"position","Position"}};
inline constexpr Choice HdrModes[]={{"aces","ACES"},{"reinhard","Reinhard"},{"clamp","Clamp"},
    {"no-bloom","Bloom disabled"},{"bloom","Bloom only"}};
inline constexpr Choice TaaModes[]={{"taa","TAA"},{"off","Disabled"},{"unclamped","Unclamped history"},{"velocity","Velocity"}};
inline constexpr Choice FogModes[]={{"fog","Fog"},{"off","Disabled"},{"transmittance","Transmittance"},{"scattering","Scattering"}};
inline constexpr Choice InstancingModes[]={{"draws","Individual draws"},{"instanced","Instanced draw"}};
inline constexpr Choice ReuseModes[]={{"create","Create each frame"},{"reuse","Reuse buffer"}};
inline constexpr Choice BindingModes[]={{"material","Bind per material"},{"array","Fixed four-texture array"}};
inline constexpr Choice RecordingModes[]={{"serial","Serial"},{"parallel","Parallel"}};
inline constexpr Choice GraphModes[]={{"manual","Manual barriers"},{"graph","RenderGraph"}};
inline constexpr Choice SceneModes[]={{"","Configured scene / custom file"},{"exterior","Bistro exterior"},
    {"interior","Bistro wine interior"},{"emerald","Emerald Square"},{"comparison","Shared Sponza comparison"}};
inline constexpr Choice ShadowQualityModes[]={{"low","Low"},{"medium","Medium"},{"high","High"}};

inline constexpr ArgumentControl ProfilingArgs[]={Frames,Validation,
    Integer("Frames in flight","--flight","2",1,4),Integer("Fragment work","--work","96",0,2048)};
inline constexpr ArgumentControl ReverseDepthArgs[]={Frames,Validation,Capture,
    Flag("Reverse Z","--reverse"),Flag("Disable depth test","--no-depth-test"),Flag("Disable depth writes","--no-depth-write"),
    Number("Near plane","--near","0.1",.000001,1000000),Number("Far plane (greater than near)","--far","100000",.000002,1000000000)};
inline constexpr ArgumentControl DepthSamplingArgs[]={Frames,Validation,
    Select("Depth display","--mode","split",DepthModes),Number("Near plane","--near","0.1",.000001,1000000),
    Number("Far plane (greater than near)","--far","100",.000002,1000000000),Flag("Wait after each pass","--wait")};
inline constexpr ArgumentControl StencilArgs[]={Frames,Validation,Capture,
    Select("Stencil mode","--mode","mask",StencilModes),Integer("Stencil reference","--reference","1",0,255)};
inline constexpr ArgumentControl BarrierArgs[]={Frames,Validation,
    Integer("Frames in flight","--flight","2",1,4),Flag("Wait after each producer","--wait")};
inline constexpr ArgumentControl PresentArgs[]={Frames,Validation,Capture,
    Integer("Frames in flight","--flight","2",1,4),Select("Present mode","--mode","fifo",PresentModes)};
inline constexpr ArgumentControl PbrArgs[]={Frames,Validation,Capture,FixedTime,
    Select("Lighting mode","--mode","pbr",PbrModes),Roughness,Metallic,Exposure};
inline constexpr ArgumentControl ShadowArgs[]={Frames,Validation,Capture,FixedTime,
    Select("Shadow type","--shadow-type","directional",ShadowTypes),Select("Shadow display","--mode","shadow",ShadowModes),
    Number("Shadow bias","--bias","0.0015",0,.1),Integer("PCF radius","--pcf","1",0,2),
    Number("Cascade split","--split","8",4,16),Number("Light height","--light-height","4",2,8),Exposure};
inline constexpr ArgumentControl ClusterArgs[]={Frames,Validation,FixedTime,
    Select("Lighting mode","--mode","clustered",ClusterModes),Integer("Light count","--lights","64",1,64),Exposure};
inline constexpr ArgumentControl DeferredArgs[]={Frames,Validation,Capture,FixedTime,
    Select("Display","--mode","lit",DeferredModes),Integer("Light count","--lights","24",0,64),Roughness,Metallic,Exposure};
inline constexpr ArgumentControl HdrArgs[]={Frames,Validation,FixedTime,
    Select("Tone map / bloom","--mode","aces",HdrModes),Number("Bloom threshold","--threshold","1",0,20),
    Number("Bloom strength","--bloom","0.35",0,5),Number("Emission","--emission","12",1,100),Exposure};
inline constexpr ArgumentControl TaaArgs[]={Frames,Validation,Capture,FixedTime,
    Select("Temporal mode","--mode","taa",TaaModes),Number("History weight","--history-weight","0.9",0,.99)};
inline constexpr ArgumentControl FogArgs[]={Frames,Validation,FixedTime,
    Select("Fog display","--mode","fog",FogModes),Number("Density","--density","0.22",0,3),
    Integer("March steps","--steps","64",8,256),Number("Height falloff","--height-falloff","0.7",0,4),Exposure};
inline constexpr ArgumentControl InstancingArgs[]={Frames,Validation,Capture,
    Select("Draw mode","--mode","instanced",InstancingModes)};
inline constexpr ArgumentControl ReuseArgs[]={Frames,Validation,Select("Buffer lifetime","--mode","reuse",ReuseModes)};
inline constexpr ArgumentControl BindingArgs[]={Frames,Validation,Capture,
    Select("Texture bindings","--mode","array",BindingModes)};
inline constexpr ArgumentControl BasicAdvancedArgs[]={Frames,Validation};
inline constexpr ArgumentControl GpuCullingArgs[]={Frames,Validation,Integer("Object count","--objects","1600",1,4194240)};
inline constexpr ArgumentControl HeadlessRecordingArgs[]={
    Select("Recording mode","--mode","parallel",RecordingModes),Integer("Recording workers (0 = hardware)","--workers","2",0,64),
    Integer("Frame count","--frames","3",1,U32Max),Validation,Capture};
inline constexpr ArgumentControl GraphArgs[]={Frames,Validation,Select("Barrier mode","--mode","graph",GraphModes)};
inline constexpr ArgumentControl EditorArgs[]={Path("Project","--project="),Path("Output capture (PPM)","--capture="),
    Integer("Frame limit (omit for unlimited)","--frames=","32",1,U32Max,true)};
inline constexpr ArgumentControl ArenaArgs[]={Path("Kenney Tiny Dungeon directory","--assets"),
    Integer("Object count","--objects","6000",100,100000),Flag("Start paused","--paused")};
inline constexpr ArgumentControl ShowcaseCullingArgs[]={Frames,Validation,Integer("Object count","--objects","1600",1,U32Max)};
inline constexpr ArgumentControl CinematicArgs[]={
    Select("Scene preset","","",SceneModes,false,true),Path("Custom scene file (use configured/custom preset)","","",true),
    Path("Actor file, or knight","--actor"),Number("Actor size","--actor-size","2.4",.001,1000000,true),
    Number("Actor position XYZ","--actor-position","0 0 0",-1000000,1000000,true,3),
    Integer("Actor clip","--actor-clip","0",0,U32Max,true),Path("Camera path CSV","--path"),
    Number("Start time (seconds)","--start","0",0,1000000),Flag("Start paused","--paused"),
    Flag("Skip spatial probe bake","--no-probes"),Select("Shadow quality","--shadow-quality","high",ShadowQualityModes),
    Number("Directional shadow distance (0 = fit)","--shadow-distance","0",0,1000000),
    Number("Cascade split","--shadow-split","0.65",0,1),
    Integer("Shadow lights: directional / point / spot","--shadow-lights","4 16 16",0,16,false,3),
    Integer("Transmission layers","--glass-layers","2",1,2),Number("Point/spot source radius","--light-radius","0",0,1000000,true),
    Path("Export camera CSV (runs without rendering)","--export-camera")};

inline constexpr ActionControl CameraActions[]={
    {"Forward",Key::W,true},{"Back",Key::S,true},{"Left",Key::A,true},{"Right",Key::D,true}};
inline constexpr ActionControl GpuCullingActions[]={
    {"GPU / CPU path",Key::U},{"Frustum culling",Key::C},{"Pause / play",Key::Space}};
inline constexpr ActionControl ArenaActions[]={
    {"Pause / play",Key::Space},{"Restart",Key::R},{"Overview",Key::O},{"CPU culling",Key::Num4},
    {"Instancing",Key::Num5},{"Parallel recording",Key::Num6},{"GPU culling",Key::U},{"VSync",Key::V},
    {"Console metrics",Key::M},{"Capture A",Key::F5},{"Capture B",Key::F6},{"Clear comparison",Key::F7},
    {"Profiler",Key::F11},
#ifdef DY_RENDERDOC_ENABLED
    {"RenderDoc capture",Key::F12},
#endif
};
inline constexpr ActionControl ShowcaseCullingActions[]={
    {"Pause / play",Key::Space},{"CPU frustum culling",Key::Num4},{"GPU / CPU path",Key::U},
    {"Indexed LOD",Key::L},{"Hi-Z occlusion",Key::O},{"Profiler",Key::F11},
#ifdef DY_RENDERDOC_ENABLED
    {"RenderDoc capture",Key::F12},
#endif
};
inline constexpr ActionControl CinematicActions[]={
    {"Pause / play tour and actor",Key::Space},{"Restart",Key::R},{"Tour / free camera",Key::Tab},
    {"Seek back 5 seconds",Key::Left},{"Seek forward 5 seconds",Key::Right},{"First pose",Key::Home},{"Last pose",Key::End},
    {"Forward",Key::W,true},{"Back",Key::S,true},{"Left",Key::A,true},{"Right",Key::D,true},
    {"Down",Key::Q,true},{"Up",Key::E,true},{"Faster movement",Key::LeftShift,true},
    {"Wind",Key::N},{"Lighting",Key::Num1},{"Shadows",Key::Num2},{"HDR",Key::Num3},
    {"CPU culling",Key::Num4},{"Instancing",Key::Num5},{"Parallel recording",Key::Num6},
    {"GPU frustum culling",Key::U},{"Hi-Z occlusion",Key::O},{"Ambient occlusion",Key::Num7},
    {"Screen-space GI",Key::Num8},{"Screen-space reflections",Key::Num9},{"Temporal AA",Key::Num0},
    {"Bloom",Key::B},{"Fog",Key::F},{"Sharpening",Key::K},{"Clustered lights",Key::J},
    {"Soft shadows / fixed PCF",Key::P},{"One / two glass layers",Key::F10},{"Cached spatial probes",Key::I},
    {"Transparency",Key::T},{"Next surface view",Key::V},{"Advanced raster",Key::G},
    {"Direct lighting",Key::F1},{"Diffuse IBL",Key::F2},{"Specular IBL",Key::F3},{"Sky background",Key::F4},
    {"Indexed LOD",Key::L},{"LOD colors",Key::C},{"Wireframe",Key::F9},
    {"Reduce LOD pixel error",Key::LeftBracket},{"Increase LOD pixel error",Key::RightBracket},
    {"Reduce exposure",Key::Minus},{"Increase exposure",Key::Equal},{"Console metrics",Key::M},
    {"Next comparison option",Key::F8},{"Capture A",Key::F5},{"Capture B",Key::F6},{"Clear comparison",Key::F7},
    {"Profiler",Key::F11},
#ifdef DY_RENDERDOC_ENABLED
    {"RenderDoc capture",Key::F12},
#endif
};
inline constexpr ActionControl EditorActions[]={{"Profiler",Key::F11},
#ifdef DY_RENDERDOC_ENABLED
    {"RenderDoc capture",Key::F12},
#endif
};

constexpr const char* PassiveNote="The original entry has no CLI or key controls. The host owns Back/stop; Escape injection is not a substitute.";
constexpr const char* ReloadNote="Apply startup options by reloading the original module in the host. A frame limit of zero runs until the host stops it.";

template<std::size_t N>
constexpr ModuleControls Args(const char* target,const ArgumentControl (&args)[N],const char* notes=ReloadNote)
{ return {target,args,N,nullptr,0,notes}; }
template<std::size_t N,std::size_t M>
constexpr ModuleControls Both(const char* target,const ArgumentControl (&args)[N],const ActionControl (&actions)[M],const char* notes)
{ return {target,args,N,actions,M,notes}; }

inline constexpr ModuleControls Modules[]={
    {"RhiDeviceAndPresent",nullptr,0,nullptr,0,PassiveNote},
    {"RhiGraphicsPipeline",nullptr,0,nullptr,0,PassiveNote},
    {"RhiBuffersAndDrawCalls",nullptr,0,nullptr,0,PassiveNote},
    {"RhiConstantsAndBindings",nullptr,0,nullptr,0,PassiveNote},
    {"RhiTexturesAndSamplers",nullptr,0,nullptr,0,PassiveNote},
    {"RhiDepthAndBlend",nullptr,0,nullptr,0,PassiveNote},
    {"RhiRenderTargetsAndBarriers",nullptr,0,nullptr,0,PassiveNote},
    {"RhiComputePipeline",nullptr,0,nullptr,0,"No original CLI/key controls; compute support is required. Back/stop belongs to the host."},
    {"RhiSubmissionAndFrames",nullptr,0,nullptr,0,PassiveNote},
    {"RhiParallelRecording",nullptr,0,nullptr,0,PassiveNote},
    {"RhiRenderGraph",nullptr,0,nullptr,0,PassiveNote},
    {"DyfPrimitives",nullptr,0,nullptr,0,PassiveNote},
    {"DyfImage",nullptr,0,nullptr,0,PassiveNote},
    {"DyfViewport",nullptr,0,nullptr,0,PassiveNote},
    {"DyfGeometryCamera",nullptr,0,CameraActions,std::size(CameraActions),
        "Live movement uses held WASD. Mouse-look also requires right-button state and pointer delta; touch look must relay that gesture, not a fake keyboard key."},
    {"DyfMeshMaterial",nullptr,0,nullptr,0,PassiveNote},
    {"DyfLight",nullptr,0,nullptr,0,PassiveNote},
    {"DyfModelAnim",nullptr,0,nullptr,0,"The original Fox animation entry has no CLI/clip keys. Use the workbench's actual model controls for clip, time, speed or deformation selection."},
    {"DyfScene",nullptr,0,nullptr,0,PassiveNote},
    {"DyfText",nullptr,0,nullptr,0,PassiveNote},
    Both("DyEditor",EditorArgs,EditorActions,"This module already owns its GUI. Joined --project=, --capture= and --frames= syntax is required; omit the optional frame limit for unlimited execution."),
    Args("AdvancedProfiling",ProfilingArgs),
    Args("AdvancedDepthReverseZ",ReverseDepthArgs,"Startup/reload settings. Far must exceed near. A capture without an explicit finite frame limit defaults to one frame."),
    Args("AdvancedDepthSampling",DepthSamplingArgs,"Startup/reload settings. Far must exceed near; this entry does not accept --capture."),
    Args("AdvancedStencilMask",StencilArgs,"Startup/reload mask/outline mode; source circle writes reference 1. Capture requires a finite run."),
    Args("AdvancedResourceBarriers",BarrierArgs),
    Args("AdvancedFramesAndPresent",PresentArgs,"Startup/reload settings. Present mode support is device dependent; CPU durations are not display latency. Capture requires a finite run."),
    Args("AdvancedPBRLighting",PbrArgs,"Startup/reload analytic scene. Optional fixed time freezes motion. Capture requires a finite run."),
    Args("AdvancedShadowMapping",ShadowArgs,"Startup/reload analytic shadows; cascades are two manually fitted intervals. Capture requires a finite run."),
    Args("AdvancedClusteredLighting",ClusterArgs,"Startup/reload world-space 16 x 16 x 8 clusters with 64-light capacity. Requires compute. No --capture flag."),
    Args("AdvancedDeferredLighting",DeferredArgs),
    Args("AdvancedHDRPostProcess",HdrArgs,"Startup/reload analytic HDR/bloom. This entry does not accept --capture."),
    Args("AdvancedTemporalAA",TaaArgs,"Startup/reload moving 2D pattern. Fixed time freezes motion while jitter continues. Capture defaults to 32 frames unless explicitly set."),
    Args("AdvancedVolumetricFog",FogArgs,"Startup/reload procedural single scattering. This entry does not accept --capture."),
    Args("AdvancedInstancingBatching",InstancingArgs,"Startup/reload mode; identical 256 quads. A live collector also registers example14.instancing, but GUI availability must not depend on that collector."),
    Args("AdvancedResourceReuse",ReuseArgs,"Startup/reload mode; identical uploads and instanced picture. A live collector also registers example15.reuse. No --capture flag."),
    Args("AdvancedBindlessMaterials",BindingArgs,"Startup/reload fixed four-texture array, not general bindless. Only array mode needs DescriptorIndexing; material mode is the fallback."),
    Args("AdvancedIndirectDrawing",BasicAdvancedArgs,"Original entry has only frames/validation options. Compute, indirect draw and indirect dispatch are required. Back/stop belongs to the host."),
    Both("AdvancedGPUCulling",GpuCullingArgs,GpuCullingActions,"Startup object count; live U/C/Space actions. GPU-selected visible counts are not read back."),
    Args("AdvancedMultithreadRecording",HeadlessRecordingArgs,"Finite headless command task, not a visual scene. Frames must be positive; workers 0 selects hardware concurrency. Show completion/output in the host."),
    {"AdvancedAsyncCompute",nullptr,0,nullptr,0,"Unimplemented status-only entry; keep disabled in the presentation host."},
    Args("AdvancedRenderGraph",GraphArgs),
    Args("AdvancedMeshShaderBasics",BasicAdvancedArgs,"Startup frames/validation only. Requires mesh-shader support. Unsupported devices return 77."),
    {"AdvancedMeshletCullingLOD",nullptr,0,nullptr,0,"Unimplemented status-only entry; keep disabled in the presentation host."},
    Both("ShowcaseArena",ArenaArgs,ArenaActions,"Startup assets/object count; live scene and renderer keys. Assets directory must contain Tiles/. Back/stop belongs to the host."),
    Both("ShowcaseCulling",ShowcaseCullingArgs,ShowcaseCullingActions,"Startup object count; live culling/LOD/Hi-Z/pause. Unsupported GPU visibility retains the CPU path."),
    Both("ShowcaseCinematic",CinematicArgs,CinematicActions,
        "Startup/reload scene and actor options; live keys retain the original scene logic. Emit only one positional scene: preset or custom file. --shadow-lights consumes D/P/S with maxima 4/16/16 (clamp the first component to 4). --actor-position consumes XYZ. --no-probes requires reload; I only toggles already cached probes. --export-camera is an explicit file-export action without rendering. Keep --no-gui internal to host routing; camera look needs pointer/button gestures.")
};
}

[[nodiscard]] inline constexpr const ModuleControls* Find(std::string_view target)
{
    for(const auto& module:detail::Modules) if(target==module.target) return &module;
    return nullptr;
}
}
