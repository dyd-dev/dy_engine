#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#define private public
#include "dyf/Renderer.h"
#undef private
#include "dyf/RendererInternal.h"
#include "dyf/ImageDds.h"
#include "dyf/Scene.h"
#include "dyf/Camera.h"
#include <meshoptimizer.h>

namespace {
void Check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
using Vertex=dyf::RendererVertex;
using ShadowVertex=dyf::Renderer::Impl::CompactShadowVertex;
using Triangle=std::array<std::array<uint32_t,6>,3>;

template<class V> std::vector<Triangle> Triangles(const std::vector<V>& vertices,const std::vector<uint32_t>& indices)
{
    std::vector<Triangle> result(indices.size()/3);
    for(size_t i=0;i<indices.size();++i) {
        const auto& v=vertices[indices[i]];
        const std::array<float,6> attributes={v.px,v.py,v.pz,v.u,v.v,
            [&] {if constexpr(std::is_same_v<V,Vertex>)return v.ca;else return v.alpha;}()};
        std::memcpy(result[i/3][i%3].data(),attributes.data(),sizeof(attributes));
    }
    std::sort(result.begin(),result.end());return result;
}

void ShadowCache()
{
    constexpr uint32_t side=64;
    std::vector<Vertex> vertices((side+1)*(side+1));
    for(uint32_t y=0;y<=side;++y)for(uint32_t x=0;x<=side;++x) {
        auto& v=vertices[y*(side+1)+x];v.px=float(x);v.py=float(y);v.pz=.01f*float(x%3);
        v.u=float(x)/side;v.v=float(y)/side;v.ca=.25f+.75f*float((x+y)%7)/6;
    }
    std::vector<uint32_t> ordered;
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x) {
        const uint32_t a=y*(side+1)+x,b=a+1,c=a+side+1,d=c+1;
        ordered.insert(ordered.end(),{a,b,c,b,d,c});
    }
    std::vector<uint32_t> indices;
    for(size_t i=0;i<ordered.size()/3;++i) {
        const auto start=((i*4051)%(ordered.size()/3))*3;
        indices.insert(indices.end(),ordered.begin()+start,ordered.begin()+start+3);
    }
    std::vector<ShadowVertex> compact;std::vector<uint32_t> optimized;
    Check(dyf::Renderer::Impl::BuildCompactShadowGeometry(vertices,indices,compact,optimized),"compact shadow build");
    Check(Triangles(vertices,indices)==Triangles(compact,optimized),"shadow ordered corners/UV/alpha changed");
    const auto before=meshopt_analyzeVertexCache(indices.data(),indices.size(),vertices.size(),16,0,0);
    const auto after=meshopt_analyzeVertexCache(optimized.data(),optimized.size(),compact.size(),16,0,0);
    std::printf("Shadow transformed vertices: before=%u after=%u\n",before.vertices_transformed,after.vertices_transformed);
    Check(after.vertices_transformed<before.vertices_transformed*.7,"shadow cache misses did not improve by 30 percent");

    // Coincident positions with different alpha/UV remain distinct, including duplicates and degenerates.
    vertices[1]=vertices[0];vertices[1].u=1;vertices[1].ca=.5f;
    indices={0,1,2,2,1,0,0,1,2,1,1,2};
    Check(dyf::Renderer::Impl::BuildCompactShadowGeometry(vertices,indices,compact,optimized),"seam compact build");
    Check(Triangles(vertices,indices)==Triangles(compact,optimized),"shadow seam/winding/duplicate preservation");
    Check(!dyf::Renderer::Impl::BuildCompactShadowGeometry(vertices,{0,1,UINT32_MAX},compact,optimized),"invalid shadow index accepted");
    Check(dyf::Renderer::Impl::BuildCompactShadowGeometry({}, {},compact,optimized) && compact.empty() && optimized.empty(),"empty shadow input");
    vertices.resize(70002);indices.resize(vertices.size());
    for(uint32_t i=0;i<vertices.size();++i){vertices[i].px=float(i);indices[i]=i;}
    Check(dyf::Renderer::Impl::BuildCompactShadowGeometry(vertices,indices,compact,optimized),"large compact shadow");
    Check(optimized.size()==indices.size() && *std::max_element(optimized.begin(),optimized.end())>UINT16_MAX,"R32 shadow indices were truncated");
}

void ShadowVolumes()
{
    using Volume=dyf::Renderer::Impl::ShadowCasterVolume;
    namespace Math=dyf::Math;
    dyf::Camera camera;camera.view=Math::float4x4::Identity();
    Check(camera.SetPerspective(1,1.57079632679f,1,10),"shadow volume camera");
    const auto outside=[](const Volume& volume,Math::float3 p) {
        return volume.Outside({p,p,true},{std::abs(double(p.x)),std::abs(double(p.y)),std::abs(double(p.z))});
    };
    Volume volume;volume.Build(camera,1,10,{0,0,1},.1);
    Check(volume.count>0 && volume.count<=volume.planes.size(),"shadow extrusion planes");
    Check(!outside(volume,{0,0,50}),"offscreen upstream caster removed");
    Check(outside(volume,{100,0,-5}),"unrelated side caster retained");
    Check(!outside(volume,{10.05f,0,-10}),"shadow filter border removed");
    for(const auto light:{Math::float3{0,0,1},Math::float3{1,0,0},Math::float3{-.3f,.7f,.2f},Math::float3{.4f,-.1f,-.9f}}) {
        volume.Build(camera,1,10,light,.1);
        const auto direction=Math::Normalize(light);
        for(uint32_t i=0;i<1000;++i) {
            const float z=-1.f-9.f*float(i%31)/30;
            const float x=(-z)*(-.999f+1.998f*float((i*37)%101)/100);
            const float y=(-z)*(-.999f+1.998f*float((i*43)%103)/102);
            const auto p=Math::float3{x,y,z}+direction*float((i*7)%17000);
            Check(!outside(volume,p),"receiver-to-light ray was culled");
        }
    }
    volume.Build(camera,1,10,{0,0,0},.1);Check(volume.count==0,"invalid light did not disable volume");
    volume.Build(camera,10,1,{0,0,1},.1);Check(volume.count==0,"invalid receiver range");
    camera.view.m[3]=1;volume.Build(camera,1,10,{0,0,1},.1);
    Check(volume.count==0,"projective camera did not keep fallback");
}

void ShadowBatches()
{
    using Impl=dyf::Renderer::Impl;
    namespace Math=dyf::Math;
    dyf::Scene scene;
    std::array<dyf::EntityHandle,2> sources;
    std::vector<Impl::ShadowCaster> casters;
    for(uint32_t i=0;i<12;++i) {
        const float x=-.8f+.15f*i;
        const auto world=Math::Translation({x,0,.3f})*Math::RotationZ(.07f*i);
        auto entity=i<2 ? scene.Add(dyf::CreateCubeMesh(.04f),{},world) : scene.AddInstance(sources[i%2],world);
        Check(bool(entity),"packed shadow fixture entity");if(i<2)sources[i]=entity;
        Impl::ShadowCaster caster;caster.entity=i;caster.mesh=i%2;
        caster.material=dyf::ToIndex(scene.GetEntityMaterial(static_cast<dyf::EntityID>(i)));
        caster.arena=caster.sortSafe=true;
        caster.worldBounds.bounds={{x-.03f,-.03f,.27f},{x+.03f,.03f,.33f},true};
        caster.worldBounds.magnitude={std::abs(x)+.03,.03,.33};casters.push_back(caster);
    }
    Impl::ShadowData shadows;shadows.viewCount=2;
    shadows.constants.lightViewProjectionMatrix[0]=Math::float4x4::Identity();
    shadows.constants.lightViewProjectionMatrix[1]=Math::float4x4::Identity();
    shadows.constants.lightViewProjectionMatrix[1].m[12]=-1;
    Impl::ShadowInstanceData packed;
    Check(packed.Build(scene,shadows,casters,true),"nonconsecutive shadow batching unavailable");
    Check(packed.views.size()==2 && packed.views[0].count==2 && packed.views[1].count==2 &&
        packed.entities.size()==18,"nonconsecutive shadows did not reduce 18 draws to four");
    const auto verify=[&] {
        Check(packed.matrices.size()==packed.entities.size()*2,"packed shadow instance ABI");
        for(uint32_t view=0;view<2;++view) {
            std::vector<uint32_t> actual;
            const auto range=packed.views[view];
            for(uint32_t i=range.first;i<range.first+range.count;++i) {
                const auto& batch=packed.draws[i];
                for(uint32_t j=0;j<batch.count;++j) {
                    const auto offset=batch.first+j,id=packed.entities[offset];actual.push_back(id);
                    Check(casters[id].mesh==casters[batch.caster].mesh,"packed batch mixes meshes");
                    Check(std::memcmp(packed.matrices[offset*2].m,scene.GetTransform(static_cast<dyf::EntityID>(id)).m,64)==0,
                        "packed matrix differs from original entity");
                    const Math::float4x4 zero={};
                    Check(std::memcmp(packed.matrices[offset*2+1].m,zero.m,64)==0,"packed shadow stride padding");
                }
            }
            std::sort(actual.begin(),actual.end());std::vector<uint32_t> expected;
            for(uint32_t id=view?6u:0u;id<12;++id)expected.push_back(id);
            Check(actual==expected,"packed light-frustum caster set changed or duplicated");
        }
    };
    verify();
    for(uint32_t i=0;i<12;++i) {
        auto material=scene.GetEntity(static_cast<dyf::EntityID>(i)).GetMaterial();
        material.alphaMode=dyf::MaterialAlphaMode::Mask;
        if(i==2)material.alphaCutoff=.25f;
        if(i==4)material.baseColor.w=.75f;
        if(i==8)material.doubleSided=true;
        Check(scene.GetEntity(static_cast<dyf::EntityID>(i)).SetMaterial(material),"packed mask fixture material");
    }
    casters[6].coverageFlags=1;casters[10].mirrored=true;
    Check(packed.Build(scene,shadows,casters,true) && packed.views[0].count==7 && packed.views[1].count==4,
        "incompatible mask alpha/cutoff/texture flags/sides/mirroring merged");
    verify();
    const auto view=packed.views[0];
    for(uint32_t b=view.first;b<view.first+view.count;++b) {
        const auto& batch=packed.draws[b];
        if(casters[batch.caster].mesh==0)Check(batch.count==1,"distinct MASK coverage shared draw constants");
    }
    casters[0].sortSafe=false;
    Check(!packed.Build(scene,shadows,casters,true),"unsafe numeric inputs did not retain legacy shadows");
    casters[0].sortSafe=true;casters[0].arena=false;
    Check(!packed.Build(scene,shadows,casters,true),"dynamic/nonarena geometry did not retain legacy shadows");
    casters[0].arena=true;
    shadows.constants.lightViewProjectionMatrix[1].m[0]=std::numeric_limits<float>::quiet_NaN();
    Check(!packed.Build(scene,shadows,casters,true),"invalid shadow projection was batched");
}

template<class T,class=void> struct HasRelease:std::false_type {};
template<class T> struct HasRelease<T,std::void_t<decltype(std::declval<T&>().ReleaseDecodedPixels())>>:std::true_type {};
template<class T> void ImageCache()
{
    Check(HasRelease<T>::value,"explicit decoded DDS cache release is unavailable");
    if constexpr(HasRelease<T>::value) {
        for(int bc:{1,3,5}) {
            const bool bc3=bc==3,bc5=bc==5;
            const size_t blockBytes=bc3||bc5?16:8;
            std::vector<uint8_t> encoded(128+blockBytes*3,0);
            const auto put=[&](size_t offset,uint32_t value){std::memcpy(encoded.data()+offset,&value,4);};
            put(0,0x20534444);put(4,124);put(8,0x21007);put(12,4);put(16,4);put(28,3);
            put(76,32);put(80,bc5?0x80000004u:4u);put(84,bc5?0x32495441:bc3?0x35545844:0x31545844);
            if(bc5)put(88,0x59583241);
            put(108,0x401008);
            for(size_t start=128;start<encoded.size();start+=blockBytes) {
                if(bc5){encoded[start]=180;encoded[start+1]=140;encoded[start+8]=100;encoded[start+9]=80;continue;}
                if(bc3){encoded[start]=231;encoded[start+1]=17;encoded[start+2]=0x49;}
                const size_t color=start+(bc3?8:0);encoded[color]=0;encoded[color+1]=0xf8;encoded[color+2]=0xe0;encoded[color+3]=7;
            }
            T image;Check(dyf::LoadImage(encoded.data(),encoded.size(),image),"DDS cache fixture load");
            Check(dyf::Detail::ImageDdsAccess::Get(image)!=nullptr,"fixture has no original DDS (including BC5 normals)");
            const auto expected=image.GetPixels();T copy=image;
            Check(image.ReleaseDecodedPixels()>=expected.size(),"DDS cache released no bytes");
            Check(image.IsValid() && copy.IsValid(),"released DDS became invalid");
            Check(copy.ReleaseDecodedPixels()==0,"Image copies do not share the decoded cache");
            Check(copy.GetPixels()==expected && image.GetPixels()==expected,"DDS lazy reconstruction changed RGBA");
            Check(image.GetPixels().data()==copy.GetPixels().data(),"rehydrated copies use different storage");
            Check(image.ReleaseDecodedPixels()>0,"second explicit release");
            bool first=false,second=false;
            std::thread a([&]{first=image.GetPixels()==expected;}),b([&]{second=copy.GetPixels()==expected;});
            a.join();b.join();Check(first && second,"concurrent immutable DDS reads changed RGBA");
        }
        T plain(1,1,{1,2,3,4});const auto* original=plain.GetPixels().data();
        Check(plain.ReleaseDecodedPixels()==0 && original==plain.GetPixels().data(),"non-DDS pixel storage changed");
    }
}
}
int main(int argc,char** argv) try {
    const std::string mode=argc>1?argv[1]:"all";
    if(mode=="all" || mode=="shadow")ShadowCache();
    if(mode=="all" || mode=="shadow-batches")ShadowBatches();
    if(mode=="all" || mode=="shadow-volumes")ShadowVolumes();
    if(mode=="all" || mode=="image")ImageCache<dyf::Image>();
    std::puts("PASS renderer optimization invariants");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
