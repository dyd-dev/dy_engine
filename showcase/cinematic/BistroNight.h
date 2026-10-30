#pragma once
#include "dyf/Scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace showcase
{
// A deliberately dark sky for the raster night demonstration. The bundle's
// bridge HDRI is daylight and contains a sun; lowering exposure cannot remove it.
inline dyf::HdrImage BistroNightSky()
{
    constexpr uint32_t width=256,height=128;
    constexpr float pi=3.14159265358979323846f;
    std::vector<float> pixels(size_t(width)*height*4);
    for(uint32_t y=0;y<height;++y)
    {
        const float z=std::cos(pi*(y+.5f)/height);
        const float horizon=std::pow(1-std::abs(z),4.f);
        const auto base=z>0 ? dyf::Math::float3{.018f,.030f,.060f} : dyf::Math::float3{.012f,.014f,.022f};
        const auto color=base+dyf::Math::float3{.030f,.030f,.035f}*horizon;
        for(uint32_t x=0;x<width;++x)
        {
            const size_t i=(size_t(y)*width+x)*4;
            pixels[i]=color.x;pixels[i+1]=color.y;pixels[i+2]=color.z;pixels[i+3]=1;
        }
    }
    return {width,height,std::move(pixels)};
}

inline dyf::Math::float3 BistroEmitterColor(const dyf::Image& image,dyf::Math::float2 uv)
{
    if(!image.IsValid()) return {1,1,1};
    const auto width=image.GetWidth(),height=image.GetHeight();
    const uint32_t x=std::min(width-1,uint32_t((uv.x-std::floor(uv.x))*width));
    const uint32_t y=std::min(height-1,uint32_t((uv.y-std::floor(uv.y))*height));
    const size_t pixel=(size_t(y)*width+x)*4;
    const auto channel=[&](size_t c) {
        const float value=image.GetPixels()[pixel+c]/255.f;
        return image.GetColorSpace()==dyf::ColorSpace::Srgb ? std::pow(value,2.2f) : value;
    };
    return {channel(0),channel(1),channel(2)};
}

// These are showcase-authored raster lights, located on the original emitters.
// They are not claimed to be analytic lights restored from the exterior FBX.
inline void AddBistroNightLights(dyf::Scene& scene,dyf::Math::float3 openingEye)
{
    using namespace dyf;
    using Math::float3;
    struct Emitter {Math::Bounds3 bounds;float3 color{};uint32_t triangles=0;int kind=0;};
    std::vector<Emitter> emitters;
    for(uint32_t entityIndex=0;entityIndex<scene.GetEntityCount();++entityIndex)
    {
        const auto entity=static_cast<EntityID>(entityIndex);
        const auto material=scene.GetEntity(entity).GetMaterial();
        if(!material.emissiveTexture.IsValid()) continue;
        const auto name=std::filesystem::path(material.emissiveTexture.GetSourcePath()).stem().string();
        const bool stringLight=name.rfind("Paris_StringLights_01_",0)==0;
        const bool street=name.rfind("Emissive_StreetLight_",0)==0;
        const bool spotlight=name.rfind("Spotlight_Glass_",0)==0;
        const bool sign=name.rfind("Shopsign_",0)==0;
        if(!stringLight && !street && !spotlight && !sign) continue;
        const uint32_t meshIndex=ToIndex(scene.GetEntityMesh(entity));
        if(meshIndex>=scene.Meshes().size() || !scene.Meshes()[meshIndex]) continue;
        const auto& mesh=*scene.Meshes()[meshIndex];
        const auto& transform=scene.GetTransform(entity);
        std::vector<uint32_t> parents(mesh.vertices.size());
        std::iota(parents.begin(),parents.end(),0u);
        const auto root=[&](uint32_t i) {
            while(parents[i]!=i) {parents[i]=parents[parents[i]];i=parents[i];}
            return i;
        };
        const auto join=[&](uint32_t a,uint32_t b) {parents[root(b)]=root(a);};
        using PositionKey=std::array<int64_t,3>;
        std::map<PositionKey,uint32_t> positions;
        struct Triangle {std::array<uint32_t,3> indices;float3 color;};
        std::vector<Triangle> active;
        const size_t corners=mesh.indices.empty() ? mesh.vertices.size() : mesh.indices.size();
        for(size_t triangle=0;triangle+2<corners;triangle+=3)
        {
            std::array<uint32_t,3> indices;
            Math::float2 uv{};
            for(uint32_t corner=0;corner<3;++corner)
            {
                indices[corner]=mesh.indices.empty() ? uint32_t(triangle+corner) : mesh.indices[triangle+corner];
                if(indices[corner]>=mesh.vertices.size()) throw std::runtime_error("Invalid Bistro emitter triangle");
                uv.x+=mesh.vertices[indices[corner]].uv.x/3;
                uv.y+=mesh.vertices[indices[corner]].uv.y/3;
            }
            const auto color=BistroEmitterColor(material.emissiveTexture,uv);
            if(std::max({color.x,color.y,color.z})<.03f) continue;
            for(auto index:indices)
            {
                const auto p=mesh.vertices[index].position;
                const PositionKey key={std::llround(p.x*100000),std::llround(p.y*100000),std::llround(p.z*100000)};
                const auto found=positions.emplace(key,index);
                if(!found.second) join(index,found.first->second);
            }
            join(indices[0],indices[1]);join(indices[0],indices[2]);
            active.push_back({indices,color});
        }
        std::map<uint32_t,Emitter> components;
        for(const auto& triangle:active)
        {
            auto& emitter=components[root(triangle.indices[0])];
            emitter.kind=sign ? 3 : street ? 2 : spotlight ? 1 : 0;
            emitter.color=emitter.color+triangle.color;++emitter.triangles;
            for(auto index:triangle.indices)
                emitter.bounds.Include(Math::TransformPoint(transform,mesh.vertices[index].position));
        }
        for(auto& item:components)
        {
            auto& emitter=item.second;
            if(!emitter.bounds.valid || !emitter.triangles) continue;
            const float peak=std::max({emitter.color.x,emitter.color.y,emitter.color.z});
            emitter.color=emitter.color*(1/std::max(peak,.001f));
            if(street || spotlight || (stringLight && name.find("White_")!=std::string::npos))
                emitter.color={1,.67f,.34f};
            emitters.push_back(emitter);
        }
    }
    // Stable placement and bounded overlapping influence volumes. The entire
    // source mesh remains emissive even when tiny islands share one raster light.
    std::sort(emitters.begin(),emitters.end(),[](const Emitter& a,const Emitter& b) {
        const auto x=a.bounds.Center(),y=b.bounds.Center();
        if(a.kind!=b.kind) return a.kind>b.kind;
        if(x.z!=y.z) return x.z<y.z;
        if(x.y!=y.y) return x.y<y.y;
        return x.x<y.x;
    });
    std::vector<float3> placed;
    uint32_t points=0,spots=0,rects=0,discs=0,shadowSpots=0;
    for(const auto& emitter:emitters)
    {
        const auto center=emitter.bounds.Center();
        const auto span=emitter.bounds.max-emitter.bounds.min;
        const float spacing=emitter.kind==2 ? .8f : emitter.kind==0 ? .35f : .15f;
        if(std::any_of(placed.begin(),placed.end(),[&](float3 p){return Math::LengthSquared(p-center)<spacing*spacing;})) continue;
        placed.push_back(center);
        if(emitter.kind==3 && rects<4)
        {
            RectAreaLight light;
            light.position=center;light.up={0,0,1};
            const auto toCamera=openingEye-center;
            light.direction=span.x<span.y ? float3{toCamera.x<0 ? -1.f : 1.f,0,0} : float3{0,toCamera.y<0 ? -1.f : 1.f,0};
            light.position=light.position+light.direction*.04f;
            light.width=std::clamp(std::max(span.x,span.y),.15f,4.f);
            light.height=std::clamp(span.z,.15f,4.f);
            light.color=emitter.color;light.intensity=16;
            if(!scene.Add(light)) throw std::runtime_error("Bistro sign area-light creation failed");
            ++rects;
        }
        else if(emitter.kind==2 && spots<24)
        {
            SpotLight light;
            light.position=center+float3{0,0,-.1f};light.direction={0,0,-1};
            light.color=emitter.color;light.intensity=65;light.range=12;
            light.innerConeRadians=.8f;light.outerConeRadians=1.3f;light.sourceRadius=.12f;
            light.castShadow=shadowSpots<4;light.shadowStrength=.9f;
            if(!scene.Add(light)) throw std::runtime_error("Bistro street-light creation failed");
            if(light.castShadow) ++shadowSpots;
            ++spots;
        }
        else if(emitter.kind==1 && discs<1)
        {
            DiscAreaLight light;
            light.position=center+float3{0,0,-.06f};light.direction={0,0,-1};
            light.radius=std::clamp(std::max(span.x,span.y)*.5f,.08f,.4f);
            light.color=emitter.color;light.intensity=160;
            if(!scene.Add(light)) throw std::runtime_error("Bistro downlight creation failed");
            ++discs;
        }
        else if(points<160)
        {
            PointLight light;
            light.position=center;light.color=emitter.color;
            light.intensity=emitter.kind==0 ? 3.5f : 12.f;
            light.range=emitter.kind==0 ? 4.f : 6.f;light.sourceRadius=.06f;light.castShadow=false;
            if(!scene.Add(light)) throw std::runtime_error("Bistro bulb-light creation failed");
            ++points;
        }
    }
    std::fprintf(stderr,"Bistro night: %zu emitter islands; added %u bulb lights, %u street spots (%u shadowed), %u rect and %u disc LTC lights. Showcase-authored approximation.\n",
        emitters.size(),points,spots,shadowSpots,rects,discs);
}
}
