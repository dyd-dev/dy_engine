#include "LocalLightData.h"
#include "SceneAlgorithms.h"
#include "dyf/Scene.h"
#include "dyf/RHI/IDevice.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace dyf
{
namespace
{
template<class Lights>
void SelectShadowSlots(const Lights& lights,std::vector<uint32_t>& selected)
{
    selected.clear();
    selected.reserve(lights.size());
    for(uint32_t index=0;index<lights.size();++index)
        if(lights[index].enabled) selected.push_back(index);
    std::sort(selected.begin(),selected.end(),[&](uint32_t a,uint32_t b) {
        return lights[a].priority==lights[b].priority ? a<b : lights[a].priority>lights[b].priority;
    });
    if(selected.size()>16) selected.resize(16);
}

float ShadowSlot(const std::vector<uint32_t>& selected,uint32_t index)
{
    const auto found=std::find(selected.begin(),selected.end(),index);
    return found==selected.end() ? -1.f : static_cast<float>(found-selected.begin());
}
}

bool LocalLightData::Prepare(const RHI::IDevice& device,const Scene& scene,bool active)
{
    lights.clear();
    if(!active) return true;
    const auto& points=scene.PointLights();
    const auto& spots=scene.SpotLights();
    if(points.size()>UINT32_MAX || spots.size()>UINT32_MAX)
    {
        std::fputs("dyf: Local light indices exceed the renderer's 32-bit range.\n",stderr);
        return false;
    }
    const uint64_t maximumBytes=std::min<uint64_t>(UINT32_MAX,device.GetLimit(RHI::Limit::StorageBufferBytes));
    const auto maximumLights=maximumBytes/sizeof(RendererClusterLight);
    if(!maximumLights)
    {
        std::fputs("dyf: The device cannot hold a local light storage buffer.\n",stderr);
        return false;
    }
    SelectShadowSlots(points,m_points);
    SelectShadowSlots(spots,m_spots);
    lights.reserve(static_cast<size_t>(std::min(uint64_t(points.size())+spots.size(),maximumLights)));
    for(uint32_t index=0;index<points.size();++index)
    {
        const auto& light=points[index];
        if(!light.enabled || light.range<=0 || light.intensity<=0) continue;
        if(lights.size()==maximumLights)
        {
            std::fputs("dyf: Active local lights exceed the device storage-buffer limit.\n",stderr);
            return false;
        }
        lights.push_back({{light.position.x,light.position.y,light.position.z,light.range},
            {light.color.x,light.color.y,light.color.z,light.intensity},
            {0,0,-1,-1},{1,2,ShadowSlot(m_points,index),0}});
    }
    for(uint32_t index=0;index<spots.size();++index)
    {
        const auto& light=spots[index];
        if(!light.enabled || light.range<=0 || light.intensity<=0) continue;
        if(lights.size()==maximumLights)
        {
            std::fputs("dyf: Active local lights exceed the device storage-buffer limit.\n",stderr);
            return false;
        }
        const auto direction=Math::NormalizeOr(light.direction,{0,0,-1});
        const auto cone=EffectiveSpotCone(light.innerConeRadians,light.outerConeRadians);
        lights.push_back({{light.position.x,light.position.y,light.position.z,light.range},
            {light.color.x,light.color.y,light.color.z,light.intensity},
            {direction.x,direction.y,direction.z,std::cos(cone.outer)},
            {std::cos(cone.inner),3,ShadowSlot(m_spots,index),0}});
    }
    return true;
}
}
