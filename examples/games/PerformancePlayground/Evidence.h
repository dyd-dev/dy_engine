#pragma once
#include "World.h"
#include <fstream>
#include <iomanip>

namespace playground {
struct GameplayEvidence {float walkMeters=0,runMeters=0,mouseRadians=0,maxBob=0;unsigned movementKeys=0;bool walkedThroughDoor=false;};
inline void WriteTimingEvidence(std::ostream& out,const dyf::Platform::ProfilerTimingSnapshot& timing,bool shadows) {
    out<<"{\"sequence\":"<<timing.sequence<<",\"frames\":"<<timing.frameCount
       <<",\"windowMs\":"<<timing.sampleWindowMilliseconds<<",\"cpuRenderMs\":";
    if(timing.frameCount)out<<timing.cpuRenderAverageMilliseconds;else out<<"null";
    out<<",\"frameAverageMs\":";
    if(timing.frameCount)out<<timing.frameAverageMilliseconds;else out<<"null";
    const auto gpu=[&](const char* name,double milliseconds,bool ready,bool enabled) {
        const char* state=!enabled?"Unavailable":ready?"Ready":(!timing.sequence||timing.gpuTimingSupported)?"Pending":"Unsupported";
        out<<",\""<<name<<"\":{\"state\":\""<<state<<"\",\"milliseconds\":";
        if(enabled&&ready)out<<milliseconds;else out<<"null";
        out<<"}";
    };
    gpu("gpuFrame",timing.gpuFrameAverageMilliseconds,timing.hasGpuFrame,true);
    gpu("gpuMain",timing.gpuMainAverageMilliseconds,timing.hasGpuMain,true);
    gpu("gpuShadow",timing.gpuShadowAverageMilliseconds,timing.hasGpuShadow,shadows);
    out<<",\"gpuFrameSamples\":"<<timing.gpuFrameSampleCount<<"}";
}
inline void SavePpm(const std::filesystem::path& file,const dyf::Image& image) {
    std::ofstream out(file,std::ios::binary);
    if(!image.IsValid()||!out) throw std::runtime_error("Capture unavailable: "+file.string());
    out<<"P6\n"<<image.GetWidth()<<" "<<image.GetHeight()<<"\n255\n";
    const auto& rgba=image.GetPixels();
    for(size_t i=0;i<rgba.size();i+=4) out.write(reinterpret_cast<const char*>(rgba.data()+i),3);
    if(!out) throw std::runtime_error("Could not write capture");
}
inline void SaveEvidence(const std::filesystem::path& file,const Controller& player,const World& world,const dyf::Renderer& renderer,int station,const GameplayEvidence& gameplay) {
    std::ofstream out(file);if(!out) throw std::runtime_error("Could not create evidence file");
    const auto& c=renderer.GetAppliedConfig();const auto& s=renderer.GetLastRenderStatistics();
    out<<std::setprecision(12)<<"{\n\"station\":"<<station+1<<",\"position\":["<<player.position.x<<","<<player.position.y<<"],\"yaw\":"<<player.yaw
       <<",\"distanceM\":"<<player.distance<<",\"speedMps\":"<<player.speed<<",\"sprinting\":"<<(player.sprinting?"true":"false")
       <<",\"walkMeters\":"<<gameplay.walkMeters<<",\"runMeters\":"<<gameplay.runMeters<<",\"mouseRadians\":"<<gameplay.mouseRadians<<",\"maxCameraBobM\":"<<gameplay.maxBob<<",\"movementKeyMask\":"<<gameplay.movementKeys<<",\"walkedThroughDoor\":"<<(gameplay.walkedThroughDoor?"true":"false")
       <<",\"entities\":"<<world.scene.GetEntityCount()<<",\"attachments\":"<<world.AttachmentCount()
       <<",\"mainDraws\":"<<s.mainDrawCount<<",\"mainInstances\":"<<s.mainInstanceCount<<",\"frustumCulled\":"<<s.culledEntityCount
       <<",\"instancing\":"<<(c.enableInstancing?"true":"false")<<",\"culling\":"<<(c.enableFrustumCulling?"true":"false")
       <<",\"lighting\":"<<(c.lighting.enabled?"true":"false")<<",\"shadows\":"<<(c.lighting.shadows?"true":"false")<<",\"parallel\":"<<(c.enableParallelRenderGraph?"true":"false")
       <<",\"vsync\":"<<(c.vsync?"true":"false")<<",\"recordingThreads\":"<<c.recordingThreadCount<<",\n\"timing\":";
    WriteTimingEvidence(out,renderer.GetProfilerTimingSnapshot(),c.lighting.enabled&&c.lighting.shadows);
    // Public timing windows do not expose the old collector's frame/source/revision provenance.
    out<<",\"liveDiagnostics\":{\"state\":\"Unavailable\",\"reason\":\"Not exposed by the public main API\"}";
    out<<"\n}\n";if(!out)throw std::runtime_error("Could not write evidence");
}
}
