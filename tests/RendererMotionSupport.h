#pragma once
#include "RendererPerformanceSupport.h"
#include <cstring>

namespace RendererPerformance
{
// Shared by the old-library benchmark, current benchmark and native pixel tests.
// Fixed simulation ticks keep motion identical regardless of rendering speed.
struct MovingWorkload : dyf::Scene
{
    std::vector<dyf::EntityHandle> entities;
    uint32_t rowCount;
    explicit MovingWorkload(uint32_t count) : rowCount((count+31)/32)
    {
        dyf::MaterialDesc material;
        material.baseColor={.6f,.8f,.35f,1};
        const auto mesh=CreateMesh(dyf::CreateCubeMesh(.28f));
        const auto mat=CreateMaterial(material);
        entities.reserve(count);
        for(uint32_t i=0;i<count;++i)
            entities.push_back(GetEntity(CreateEntity(mesh,mat,Matrix(i,0))));
    }
    Math::float4x4 Matrix(uint32_t i,uint32_t tick) const
    {
        const float t=float(tick)/60.f, phase=float(i)*.173f;
        const float rows=float(rowCount);
        return Math::Translation({(float(i%32)-15.5f)*.45f+.07f*std::sin(t*1.3f+phase),
            (float(i/32)-(rows-1)*.5f)*.45f+.07f*std::cos(t*1.1f+phase),
            .06f*std::sin(t+phase)}) * Math::RotationZ(t*.7f+phase) * Math::RotationY(t*.4f+phase*.5f);
    }
    void Update(uint32_t tick)
    {
        for(uint32_t i=0;i<entities.size();++i)
            if(!entities[i].SetTransform(Matrix(i,tick)))
                throw std::runtime_error("Moving entity transform update failed");
    }
    void VerifyFrame(uint32_t tick) const
    {
        for(uint32_t i=0;i<entities.size();++i)
        {
            const auto actual=entities[i].GetTransform(), expected=Matrix(i,tick);
            Check(std::memcmp(actual.m,expected.m,sizeof(actual.m))==0,"Motion tick differs from Scene transform");
            if(tick) { const auto previous=Matrix(i,tick-1);
                Check(std::memcmp(previous.m,actual.m,sizeof(actual.m))!=0,"Entity did not move between ticks"); }
        }
    }
    dyf::Camera Camera() const
    { return TopCamera(std::max(18.f,float((entities.size()+31)/32)*.45f+2),12); }
};
}
