#include "EditorScene.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace editor
{
using namespace dyf;
namespace M = dyf::Math;
constexpr float Pi = 3.14159265359f;
const char* PrimitiveName(Primitive p) { return p==Primitive::Sphere ? "Sphere" : p==Primitive::Floor ? "Floor" : "Cube"; }
M::float4x4 Transform(const Object& o)
{
    constexpr float radians = Pi/180;
    return M::Translation({o.position[0],o.position[1],o.position[2]})
        * M::RotationZ(o.rotation[2]*radians)*M::RotationY(o.rotation[1]*radians)*M::RotationX(o.rotation[0]*radians)
        * M::Scaling({o.scale[0],o.scale[1],o.scale[2]});
}
static MeshData Sphere()
{
    MeshData mesh;
    constexpr uint32_t rows=24, columns=40;
    for(uint32_t y=0;y<=rows;++y) for(uint32_t x=0;x<=columns;++x)
    {
        const float theta=Pi*y/rows, phi=2*Pi*x/columns;
        Vertex v;
        v.normal={std::sin(theta)*std::cos(phi),std::sin(theta)*std::sin(phi),std::cos(theta)};
        v.position=v.normal*.5f; v.uv={float(x)/columns,float(y)/rows};
        v.tangent={-std::sin(phi),std::cos(phi),0,1};
        mesh.vertices.push_back(v);
    }
    for(uint32_t y=0;y<rows;++y) for(uint32_t x=0;x<columns;++x)
    {
        const uint32_t a=y*(columns+1)+x,b=a+columns+1;
        for(auto i:{a,b,a+1,a+1,b,b+1}) mesh.indices.push_back(i);
    }
    return mesh;
}
static std::string Encode(const std::vector<Object>& objects)
{
    std::ostringstream out; out.imbue(std::locale::classic());
    out<<"DYSCENE 1\n"<<objects.size()<<'\n'<<std::setprecision(std::numeric_limits<float>::max_digits10);
    for(const auto& o:objects)
    {
        out<<int(o.primitive)<<' '<<std::quoted(o.name);
        for(const auto& field:{o.position,o.rotation,o.scale}) for(float v:field) out<<' '<<v;
        for(float v:o.color) out<<' '<<v;
        out<<' '<<o.roughness<<' '<<o.metallic<<'\n';
    }
    return out.str();
}
static bool Valid(const Object& o)
{
    if(int(o.primitive)<0 || int(o.primitive)>2 || o.name.empty() || o.name.size()>127
        || o.name.find_first_of("\r\n\t")!=std::string::npos || o.name.find('\0')!=std::string::npos) return false;
    for(float v:o.position) if(!std::isfinite(v) || std::abs(v)>10000) return false;
    for(float v:o.rotation) if(!std::isfinite(v) || std::abs(v)>360000) return false;
    for(float v:o.scale) if(!std::isfinite(v) || v<.01f || v>1000) return false;
    for(float v:o.color) if(!std::isfinite(v) || v<0 || v>1) return false;
    return std::isfinite(o.roughness) && o.roughness>=.04f && o.roughness<=1
        && std::isfinite(o.metallic) && o.metallic>=0 && o.metallic<=1;
}
Document::Document() { New(true); }
std::string Document::Contents() const { return Encode(objects); }
bool Document::Dirty() const { return Contents()!=saved; }
void Document::New(bool examples)
{
    Stop(); objects.clear(); undo.clear(); redo.clear(); path.clear(); selected=-1;
    if(examples)
    {
        Object floor; floor.primitive=Primitive::Floor;floor.name="Ground";floor.position={0,0,-.1f};
        floor.scale={12,10,.2f};floor.color={.24f,.26f,.29f,1};floor.roughness=.8f;objects.push_back(floor);
        const std::array<std::array<float,4>,5> colors={{{.07f,.55f,.2f,1},{.88f,.08f,.06f,1},{.95f,.59f,.05f,1},{.16f,.32f,.85f,1},{.62f,.25f,.68f,1}}};
        for(int i=0;i<5;++i)
        {
            Object o; o.primitive=i==1||i==4?Primitive::Cube:Primitive::Sphere;
            o.name=std::string(PrimitiveName(o.primitive))+" "+std::to_string(i+1);
            o.position={float(i-2)*1.65f,(i%2?1.f:0.f),.7f};o.scale={1.4f,1.4f,1.4f};
            o.rotation[2]=i==1?20.f:0.f;o.color=colors[i];o.roughness=.18f+.07f*i;o.metallic=.1f;objects.push_back(o);
        }
        selected=1;
    }
    saved=Contents(); built.clear();scene.reset();Sync();
}
bool Document::Commit(const std::vector<Object>& before)
{
    const auto clampSelection=[&]{selected=objects.empty()?-1:std::clamp(selected,-1,int(objects.size()-1));};
    if(playing || objects.size()>2048 || !std::all_of(objects.begin(),objects.end(),Valid))
    {objects=before;clampSelection();Sync();return false;}
    clampSelection();
    if(Encode(before)==Contents()) return false;
    undo.push_back(before);if(undo.size()>64) undo.erase(undo.begin());redo.clear();Sync();return true;
}
void Document::Add(Primitive primitive)
{
    if(playing || objects.size()>=2048) return;
    auto before=objects;Object o;o.primitive=primitive;o.name=std::string(PrimitiveName(primitive))+" "+std::to_string(objects.size()+1);
    if(primitive==Primitive::Floor){o.scale={8,8,.2f};o.position={0,0,-.1f};o.roughness=.8f;}
    objects.push_back(o);selected=int(objects.size()-1);Commit(before);
}
void Document::Duplicate()
{
    if(playing || selected<0 || selected>=int(objects.size()) || objects.size()>=2048) return;
    auto before=objects;Object o=objects[selected];o.name=o.name.substr(0,119)+" copy";o.position[0]+=1;
    objects.push_back(o);selected=int(objects.size()-1);Commit(before);
}
void Document::Delete()
{
    if(playing || selected<0 || selected>=int(objects.size())) return;
    auto before=objects;objects.erase(objects.begin()+selected);selected=objects.empty()?-1:std::min(selected,int(objects.size()-1));Commit(before);
}
bool Document::Undo()
{
    if(playing || undo.empty())return false;
    redo.push_back(objects);objects=std::move(undo.back());undo.pop_back();selected=objects.empty()?-1:std::clamp(selected,0,int(objects.size()-1));Sync();return true;
}
bool Document::Redo()
{
    if(playing || redo.empty())return false;
    undo.push_back(objects);objects=std::move(redo.back());redo.pop_back();selected=objects.empty()?-1:std::clamp(selected,0,int(objects.size()-1));Sync();return true;
}
void Document::Sync()
{
    bool rebuild=!scene || built.size()!=objects.size();
    for(size_t i=0;!rebuild && i<objects.size();++i) rebuild=built[i]!=objects[i].primitive;
    if(rebuild)
    {
        // ponytail: Scene has no removal API; rebuild on structural edits, add stable entity removal if large scenes need it.
        scene=std::make_unique<Scene>();built.clear();
        static const MeshData cube=CreateCubeMesh(),sphere=Sphere();
        for(const auto& o:objects){(void)scene->Add(o.primitive==Primitive::Sphere?sphere:cube);built.push_back(o.primitive);}
        DirectionalLight key;key.direction={.4f,.6f,-1};key.intensity=3;key.shadowStrength=.6f;(void)scene->Add(key);
        DirectionalLight fill;fill.direction={-.6f,-.2f,-.4f};fill.color={.55f,.68f,1};fill.intensity=.8f;fill.castShadow=false;(void)scene->Add(fill);
    }
    for(size_t i=0;i<objects.size();++i)
    {
        Object o=objects[i];
        if(playing && o.primitive!=Primitive::Floor){o.rotation[2]+=previewTime*35;o.position[2]+=.16f*std::sin(previewTime*2+float(i));}
        MaterialDesc material;material.baseColor={o.color[0],o.color[1],o.color[2],o.color[3]};material.roughnessFactor=o.roughness;material.metallicFactor=o.metallic;
        auto entity=scene->GetEntity(static_cast<EntityID>(i));entity.SetTransform(Transform(o));entity.SetMaterial(material);
    }
}
void Document::Play(){playing=true;paused=false;previewTime=0;Sync();}
void Document::Tick(float seconds){if(playing && !paused){previewTime+=seconds;Sync();}}
void Document::Stop(){playing=false;paused=false;previewTime=0;if(scene)Sync();}
bool Document::Save(const std::filesystem::path& destination,std::string& error)
{
    error.clear();
    if(objects.size()>2048 || !std::all_of(objects.begin(),objects.end(),Valid)){error="Scene contains invalid object values.";return false;}
    const auto contents=Contents();
    auto temporary=destination;temporary+=L".tmp-"+std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
    {std::ofstream out(temporary,std::ios::binary);out.write(contents.data(),std::streamsize(contents.size()));out.close();
        if(!out){error="Cannot write scene: "+destination.string();std::error_code ec;std::filesystem::remove(temporary,ec);return false;}}
    bool replaced=false;
#ifdef _WIN32
    replaced=MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    std::error_code ec;std::filesystem::rename(temporary,destination,ec);replaced=!ec;
#endif
    if(!replaced){error="Cannot replace scene; previous save is preserved.";std::error_code ec;std::filesystem::remove(temporary,ec);return false;}
    path=destination;saved=contents;return true;
}
bool Document::Load(const std::filesystem::path& source,std::string& error)
{
    error.clear();std::error_code ec;const auto bytes=std::filesystem::file_size(source,ec);
    if(ec || bytes>4*1024*1024){error="Scene unavailable or larger than 4 MiB.";return false;}
    std::ifstream in(source,std::ios::binary);in.imbue(std::locale::classic());std::string magic;int version=0;size_t count=0;
    if(!(in>>magic>>version>>count) || magic!="DYSCENE" || version!=1 || count>2048){error="Invalid DYSCENE header or object count.";return false;}
    std::vector<Object> loaded;loaded.reserve(count);
    for(size_t i=0;i<count;++i)
    {
        Object o;int primitive=-1;in>>primitive>>std::quoted(o.name);o.primitive=static_cast<Primitive>(primitive);
        for(auto* field:{&o.position,&o.rotation,&o.scale})for(float& v:*field)in>>v;
        for(float& v:o.color)in>>v;in>>o.roughness>>o.metallic;
        if(!in || !Valid(o)){error="Invalid object at row "+std::to_string(i+1)+"; current scene preserved.";return false;}
        loaded.push_back(o);
    }
    in>>std::ws;if(!in.eof()){error="Unexpected data after scene objects.";return false;}
    Stop();objects=std::move(loaded);path=source;undo.clear();redo.clear();selected=objects.empty()?-1:0;saved=Contents();Sync();return true;
}
int Document::Pick(M::float3 origin,M::float3 direction) const
{
    float nearest=std::numeric_limits<float>::max();int result=-1;
    // ponytail: exact primitive tests with an O(n) scan; use a BVH if scene sizes make selection slow.
    for(size_t i=0;i<objects.size();++i)
    {
        M::float4x4 inverse;if(!M::Inverse(scene->GetEntity(static_cast<EntityID>(i)).GetTransform(),inverse))continue;
        const auto o=M::TransformPoint(inverse,origin),d=M::TransformVector(inverse,direction);
        float t=std::numeric_limits<float>::max();
        if(objects[i].primitive==Primitive::Sphere)
        {
            const float a=M::Dot(d,d),b=M::Dot(o,d),c=M::Dot(o,o)-.25f,disc=b*b-a*c;
            if(a>1e-12f && disc>=0){t=(-b-std::sqrt(disc))/a;if(t<0)t=(-b+std::sqrt(disc))/a;}
        }
        else
        {
            float entry=0,exit=std::numeric_limits<float>::max();bool hit=true;
            for(int axis=0;axis<3;++axis)
            {
                if(std::abs(d[axis])<1e-8f){if(o[axis]<-.5f || o[axis]>.5f)hit=false;continue;}
                float a=(-.5f-o[axis])/d[axis],b=(.5f-o[axis])/d[axis];if(a>b)std::swap(a,b);
                entry=std::max(entry,a);exit=std::min(exit,b);if(entry>exit)hit=false;
            }
            if(hit)t=entry;
        }
        if(t>=0 && t<nearest){nearest=t;result=int(i);}
    }
    return result;
}
bool SelfTest(const std::filesystem::path& directory,std::string& error)
{
    auto check=[&](bool success,const char* label){if(!success)error=label;return success;};
    Document d;d.New();d.Add(Primitive::Cube);auto initial=d.Contents();
    auto before=d.objects;d.objects[0].name="Test \"quoted\" cube";d.objects[0].position={2,3,4};d.objects[0].color={.2f,.7f,.4f,1};
    if(!check(d.Commit(before)&&d.scene->GetEntityCount()==1,"edit sync"))return false;
    const auto edited=d.Contents();d.Duplicate();
    if(!check(d.objects.size()==2 && d.scene->GetEntityCount()==2,"duplicate"))return false;
    d.Delete();if(!check(d.objects.size()==1 && d.Undo() && d.objects.size()==2 && d.Redo() && d.objects.size()==1,"delete undo redo"))return false;
    if(!check(d.Undo() && d.Undo() && d.Contents()==edited && d.Undo() && d.Contents()==initial,"edit undo"))return false;
    d.Redo();d.Play();d.Tick(.75f);const auto moving=d.scene->GetEntity(static_cast<EntityID>(0)).GetTransform();d.Stop();
    if(!check(d.Contents()==edited && moving.m[14]!=d.scene->GetEntity(static_cast<EntityID>(0)).GetTransform().m[14],"preview restore"))return false;
    std::filesystem::create_directories(directory);
    const auto file=directory/"roundtrip.dyscene",bad=directory/"malformed.dyscene";
    if(!d.Save(file,error))return false;
    Document loaded;if(!loaded.Load(file,error))return false;
    if(!check(loaded.Contents()==d.Contents() && !loaded.Dirty(),"save load roundtrip"))return false;
    {std::ofstream out(bad);out<<"DYSCENE 1\n1\n0 \"Bad\" nan\n";}
    std::string expected;if(!check(!loaded.Load(bad,expected)&&loaded.Contents()==d.Contents(),"malformed input preservation"))return false;
    const auto unchanged=d.Contents();d.objects[0].scale[0]=0;
    if(!check(!d.Save(file,expected),"invalid save rejected"))return false;
    Document reread;if(!reread.Load(file,error))return false;
    if(!check(reread.Contents()==unchanged,"failed save preserves existing file"))return false;
    loaded.New();loaded.Add(Primitive::Sphere);
    if(!check(loaded.Pick({0,-3,.5f},{0,1,0})==0 && loaded.Pick({5,-3,.5f},{0,1,0})==-1,"viewport picking"))return false;
    before=loaded.objects;loaded.objects[0].position[0]=10000;loaded.Commit(before);
    const auto boundary=loaded.Contents();loaded.Duplicate();
    if(!check(loaded.Contents()==boundary && loaded.selected==0 && loaded.scene->GetEntityCount()==1,"failed duplicate selection bounds"))return false;
    loaded.New();before=loaded.objects;loaded.objects.push_back(Object{});loaded.objects[0].scale[0]=0;loaded.selected=0;
    if(!check(!loaded.Commit(before) && loaded.objects.empty() && loaded.selected==-1,"invalid empty-scene transaction selection"))return false;
    return true;
}
}
