#pragma once
#include "dyf/Renderer.h"
#include <algorithm>
#include <cmath>

namespace showcase
{
inline constexpr unsigned PresentationShotCount=5;
inline constexpr double PresentationShotSeconds=6;
inline constexpr unsigned PresentationTextureLimit=1024;
struct PresentationFrame
{
    unsigned shot=0, phase=0;
    bool finished=false;
    bool Enabled() const { return phase==1; }
    unsigned Token() const { return shot*3+phase; }
};

class PresentationTimeline
{
public:
    void Advance(double elapsed, bool playing)
    {
        if(playing && !m_waitingForFrame && std::isfinite(elapsed) && elapsed>0)
        {
            const double nextBoundary=std::min(31.0,(std::floor(m_seconds/2)+1)*2);
            Seek(std::min(m_seconds+elapsed,nextBoundary));
        }
    }
    void Seek(double seconds)
    {
        m_seconds=std::isfinite(seconds) ? std::clamp(seconds,0.0,31.0) : 0;
        m_waitingForFrame=false;
    }
    double Seconds() const { return m_seconds; }
    void WaitForFrame() { m_waitingForFrame=true; }
    bool FrameSubmitted()
    {
        const bool waiting=m_waitingForFrame;
        m_waitingForFrame=false;
        return waiting;
    }
    bool TransitionReady() const { return m_seconds>=31; }
    PresentationFrame Frame() const
    {
        const auto shot=std::min(PresentationShotCount-1,static_cast<unsigned>(m_seconds/PresentationShotSeconds));
        const double local=m_seconds-shot*PresentationShotSeconds;
        return {shot,local<2 ? 0u : local<4 ? 1u : 2u,m_seconds>=30};
    }
private:
    double m_seconds=0;
    bool m_waitingForFrame=false;
};

enum class PresentationScene { Exterior, Interior, Emerald };
enum class PresentationFeature
{
    Lighting, Shadows, Fog, Wireframe, Depth, Surface,
    DirectLighting, Transparency, Bloom,
    Lod, Roughness, GpuPasses, CpuTiming
};
struct PresentationShot { PresentationFeature feature; const char* title; };

inline PresentationShot GetPresentationShot(PresentationScene scene, unsigned index, bool wireframeSupported=true)
{
    using F=PresentationFeature;
    static constexpr PresentationShot shots[3][PresentationShotCount]={
        {{F::Lighting,"MATERIALS & LIGHTING"},{F::Shadows,"REAL-TIME SHADOWS"},
         {F::Fog,"VOLUMETRIC FOG"},{F::Wireframe,"WIREFRAME"},
         {F::Surface,"SURFACE NORMALS"}},
        {{F::DirectLighting,"DIRECT LIGHTING"},{F::Bloom,"BLOOM"},
         {F::Transparency,"TRANSPARENCY & REFRACTION"},{F::Shadows,"POINT-LIGHT SHADOWS"},
         {F::Surface,"SURFACE NORMALS"}},
        {{F::Lod,"MESH LOD - COLORED LEVELS"},{F::Wireframe,"WIREFRAME"},
         {F::Roughness,"ROUGHNESS VIEW"},{F::GpuPasses,"GPU PASS TIMINGS"},
         {F::CpuTiming,"CPU STAGE TIMINGS"}}
    };
    auto shot=shots[static_cast<unsigned>(scene)][std::min(PresentationShotCount-1,index)];
    if(shot.feature==F::Wireframe && !wireframeSupported) shot={F::Surface,"SURFACE NORMALS"};
    return shot;
}

inline void ConfigurePresentation(dyf::RendererConfig& config)
{
    config.advanced.ambientOcclusion=false;
    config.advanced.diffuseGI=false;
    config.advanced.reflections=false;
    config.enableOcclusionCulling=false;
}

inline void ApplyPresentationFeature(dyf::RendererConfig& config, const dyf::RendererConfig& baseline,
    PresentationFeature feature, bool enabled)
{
    // Restore the complete baseline so an OFF effect or a debug view cannot leak into the next shot.
    config=baseline;
    using F=PresentationFeature;
    switch(feature)
    {
    case F::Lighting: config.lighting.enabled=enabled; break;
    case F::Shadows: config.lighting.shadows=enabled; break;
    case F::Fog:
        config.advanced.fog.enabled=enabled;
        config.advanced.fog.density=.04f;
        config.advanced.fog.heightFalloff=.025f;
        break;
    case F::Wireframe: config.debugWireframe=enabled; break;
    case F::Depth: config.advanced.surfaceDebugView=enabled ? dyf::SurfaceDebugView::Depth : dyf::SurfaceDebugView::None; break;
    case F::DirectLighting: config.lighting.direct=enabled; break;
    case F::Transparency: config.advanced.transparency=enabled; break;
    case F::Bloom:
        config.advanced.bloom=enabled;
        config.advanced.bloomStrength=.25f;
        config.advanced.bloomThreshold=.65f;
        break;
    case F::Surface: config.advanced.surfaceDebugView=enabled ? dyf::SurfaceDebugView::Normal : dyf::SurfaceDebugView::None; break;
    case F::Lod:
        config.enableLod=enabled;
        config.debugLodColor=true;
        config.lodPixelError=8;
        break;
    case F::Roughness: config.advanced.surfaceDebugView=enabled ? dyf::SurfaceDebugView::Roughness : dyf::SurfaceDebugView::None; break;
    default: break;
    }
}
}
