#include "World.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <ufbx.h>

namespace playground {
using namespace dyf;
using namespace dyf::Math;
namespace {
constexpr float Pi=3.14159265359f;
void Require(bool result,const std::string& what) {if(!result) throw std::runtime_error(what);}
float4x4 At(float3 p,float scale=1,float angle=0) {
    return Translation(p)*RotationZ(angle)*Scaling(scale)*RotationX(Pi*.5f);
}
AnimationClip LoadClip(const std::filesystem::path& path,const std::vector<ModelNode>& nodes)
{
    // Model::LoadModel requires geometry; these official animation-only FBXs use its existing ufbx dependency.
    ufbx_load_opts options={};options.ignore_geometry=true;options.ignore_embedded=true;
    options.inherit_mode_handling=UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    options.scale_helper_name.data="(scale helper)";options.scale_helper_name.length=SIZE_MAX;
    options.temp_allocator.memory_limit=32*1024*1024;options.result_allocator.memory_limit=32*1024*1024;
    ufbx_error error={};
    std::unique_ptr<ufbx_scene,decltype(&ufbx_free_scene)> source(ufbx_load_file(path.string().c_str(),&options,&error),ufbx_free_scene);
    Require(bool(source),"Animation-only FBX load: "+path.string());
    Require(source->anim_stacks.count>0,"FBX has no animation stacks");
    const auto* animation=source->anim_stacks.data[source->anim_stacks.count-1]->anim;
    const double duration=animation->time_end-animation->time_begin;
    Require(std::isfinite(duration)&&duration>0&&duration<=20,"FBX clip duration out of bounds");
    AnimationClip clip;clip.name=path.stem().string();clip.duration=float(duration);
    const int count=int(std::ceil(duration*30));
    for(uint32_t i=0;i<nodes.size();++i) {
        const auto* node=ufbx_find_node(source.get(),nodes[i].name.c_str());if(!node)continue;
        NodeAnimationTrack track;track.nodeIndex=i;
        for(int frame=0;frame<=count;++frame) {
            const float t=std::min(float(frame)/30.f,clip.duration);
            const auto value=ufbx_evaluate_transform(animation,node,animation->time_begin+t);
            const float3 translation={float(value.translation.x),float(value.translation.y),float(value.translation.z)};
            const float3 scale={float(value.scale.x),float(value.scale.y),float(value.scale.z)};
            const quat rotation={float(value.rotation.x),float(value.rotation.y),float(value.rotation.z),float(value.rotation.w)};
            track.translations.push_back({t,{},translation,{}});track.rotations.push_back({t,{},rotation,{}});track.scales.push_back({t,{},scale,{}});
        }
        clip.tracks.push_back(std::move(track));
    }
    Require(clip.tracks.size()>=10,"FBX animation joints do not match character");
    return clip;
}
}

EntityHandle World::Cube(float3 p,float3 s,float4 color,bool solid)
{
    const std::array<float,4> key={color.x,color.y,color.z,color.w};
    const auto transform=Translation(p)*Scaling(s);
    auto found=cubes.find(key);
    EntityHandle entity;
    if(found==cubes.end()) {
        MaterialDesc m;m.baseColor=color;m.roughnessFactor=.85f;
        entity=scene.Add(CreateCubeMesh(),m,transform);cubes[key]=entity;
    } else entity=scene.AddInstance(found->second,transform);
    Require(bool(entity),"Create world cube");
    if(solid) solids.push_back({p.x-s.x/2,p.y-s.y/2,p.z-s.z/2,p.x+s.x/2,p.y+s.y/2,p.z+s.z/2});
    return entity;
}

void World::Place(const std::string& asset,float3 p,float scale,float angle,bool solid)
{
    auto found=models.find(asset);
    if(found==models.end()) {
        ModelAssetID id;
        Require(LoadModelAsset(scene,(root/"Kenney"/(asset+".glb")).string(),{},&id),"Load "+asset);
        found=models.emplace(asset,id).first;
    }
    const auto first=scene.GetEntityCount();
    ModelSceneDesc desc;desc.normalize=false;desc.transform=At(p,scale,angle);
    ModelInstanceID instance;
    Require(InstantiateModel(scene,found->second,desc,&instance),"Place "+asset);
    if(solid) {
        const float inf=std::numeric_limits<float>::max();
        Box bounds={inf,inf,inf,-inf,-inf,-inf};
        for(uint32_t i=first;i<scene.GetEntityCount();++i) {
            const auto id=static_cast<EntityID>(i);
            const auto& mesh=*scene.Meshes()[ToIndex(scene.GetEntityMesh(id))];
            for(const auto& vertex:mesh.vertices) {
                const auto v=TransformPoint(scene.GetTransform(id),vertex.position);
                bounds.x0=std::min(bounds.x0,v.x);bounds.y0=std::min(bounds.y0,v.y);bounds.z0=std::min(bounds.z0,v.z);
                bounds.x1=std::max(bounds.x1,v.x);bounds.y1=std::max(bounds.y1,v.y);bounds.z1=std::max(bounds.z1,v.z);
            }
        }
        // shortcut: decorative building bounds are solid AABBs; the walkable hall has explicit openings.
        solids.push_back(bounds);
    }
}

void World::Wall(float x,float y,float length,float angle,bool window)
{
    const int count=int(std::round(length/2));
    for(int i=0;i<count;++i) {
        const float offset=-length/2+1+2*i;
        const float3 p={x-std::sin(angle)*offset,y+std::cos(angle)*offset,0};
        Place(window?"building/wall-window-square":"building/wall",p,1.5f,angle);
    }
    // Each wall module is 3 m wide after scaling.
    const float half=length*.75f;
    if(std::abs(std::sin(angle))>.5f) solids.push_back({x-half,y-.16f,0,x+half,y+.16f,3.6f});
    else solids.push_back({x-.16f,y-half,0,x+.16f,y+half,3.6f});
}

World::World(const std::filesystem::path& assets):root(assets)
{
    const float4 asphalt={.18f,.23f,.28f,1},stone={.60f,.66f,.66f,1},cyan={.12f,.73f,.78f,1},gold={.94f,.58f,.18f,1};
    Cube({0,10,-.25f},{96,96,.5f},{.24f,.32f,.30f,1});
    Cube({0,10,-.025f},{9,90,.05f},asphalt);
    Cube({0,-19,-.015f},{35,23,.07f},stone);
    Cube({18,5,-.012f},{21,30,.07f},asphalt);
    Cube({-18,12,-.015f},{27,40,.07f},stone);
    Cube({18,37,-.015f},{27,27,.07f},{.47f,.50f,.49f,1});
    Cube({-9,0,-.005f},{25,6,.09f},asphalt);
    Cube({9,-11,-.005f},{24,6,.09f},asphalt);
    Cube({13,24,-.005f},{24,6,.09f},asphalt);
    for(int i=-16;i<52;i+=4) Cube({0,float(i),.04f},{.12f,1.9f,.02f},gold);
    for(int i=0;i<4;++i) {
        const auto p=Stations[i].position;
        Cube({p.x,p.y,.045f},{3,3,.06f},i%2?gold:cyan);
        for(int stripe=0;stripe<=i;++stripe) Cube({p.x-1.f+stripe*.55f,p.y,.085f},{.17f,1.5f,.015f},{.9f,.94f,.94f,1});
    }
    for(int side:{-1,1}) for(int row=0;row<5;++row) {
        const float x=side*(row%2?37.f:39.f),y=-23.f+row*17;
        Place(row%2?"commercial/building-skyscraper-a":"commercial/building-f",{x,y,0},6,side<0?Pi:0,true);
    }
    Place("commercial/building-a",{-10,-24,0},5,0,true);
    Place("commercial/building-c",{11,-26,0},5,0,true);
    Place("commercial/building-b",{-8,-34,0},6,Pi,true);
    Place("commercial/building-c",{8,-34,0},6,Pi,true);
    Place("commercial/detail-parasol-a",{-8,-16,0},5);
    Place("commercial/detail-parasol-a",{-11,-15,0},5);

    for(int side:{-1,1}) for(int row=0;row<4;++row) {
        const float x=18+side*8.f,y=-3+row*7.f;
        Place(row%2?"commercial/building-b":"commercial/building-d",{x,y,0},5,side<0?-Pi/2:Pi/2,true);
    }
    // Add each repeated asset in contiguous groups so stock Renderer can instance the runs.
    for(const auto* asset:{"building/detail-pipe","building/gutter-vertical","commercial/detail-awning","building/plating-detailed"}) {
        for(int side:{-1,1}) for(int row=0;row<4;++row) for(int level=0;level<5;++level) for(int col=0;col<5;++col) {
            const float x=18+side*5.6f,y=-5.3f+row*7+col*1.05f,z=.5f+level*1.12f;
            const float scale=std::string(asset).find("gutter")!=std::string::npos?.48f:1.2f;
            Place(asset,{x,y,z},scale,side<0?Pi:0);++attachmentCount;
        }
    }
    for(int i=0;i<7;++i) Place("industrial/detail-tank",{28,float(-2+i*4),0},3,0,true);

    // 18 x 18 m hall, a six-metre doorway, window walls, two corridor openings.
    for(int x=-27;x<-9;x+=3) for(int y=9;y<27;y+=3) {
        Place("building/floor",{x+1.5f,y+1.5f,-.15f},1.5f);
        Place("building/roof-flat-center",{x+1.5f,y+1.5f,3.6f},1.5f);
    }
    for(float x:{-24.f,-12.f}) {
        for(int i=0;i<2;++i) Place("building/wall",{x+(i?1.5f:-1.5f),9,0},1.5f,Pi/2);
        solids.push_back({x-3,8.84f,0,x+3,9.16f,3.6f});
    }
    Place("building/wall-doorway-wide-square",{-18,9,0},1.5f,Pi/2);
    // The imported doorway has a 3 m opening; collider matches its two side pillars and lintel.
    solids.push_back({-21,8.84f,0,-19.5f,9.16f,3.6f});
    solids.push_back({-16.5f,8.84f,0,-15,9.16f,3.6f});
    solids.push_back({-19.5f,8.84f,3,-16.5f,9.16f,3.6f});
    for(float x:{-27.f,-9.f}) {
        for(int i=0;i<6;++i) Place("building/wall-window-square",{x,10.5f+3*i,0},1.5f);
        solids.push_back({x-.16f,9,0,x+.16f,27,3.6f});
    }
    for(int i=0;i<6;++i) Place("building/wall",{-25.5f+3*i,27,0},1.5f,Pi/2);
    solids.push_back({-27,26.84f,0,-9,27.16f,3.6f});
    solids.push_back({-27,9,3.6f,-9,27,3.75f});
    for(float y:{17.f,22.f}) {
        for(int i=0;i<2;++i) Place("building/wall",{-25.5f+3*i,y,0},1.5f,Pi/2);
        for(int i=0;i<2;++i) Place("building/wall",{-13.5f+3*i,y,0},1.5f,Pi/2);
        solids.push_back({-27,y-.16f,0,-21,y+.16f,3.6f});
        solids.push_back({-15,y-.16f,0,-9,y+.16f,3.6f});
    }
    for(int i=0;i<12;++i) Place("industrial/shipping-container-a",{-24.f+(i%2)*12,11.f+(i/2)*2.6f,0},3,0,true);
    for(float y:{12.f,20.f,25.f}) Cube({-18,y,3.47f},{2,.2f,.06f},{1,.82f,.38f,1});

    Place("industrial/building-a",{28,43,0},4,Pi,true);
    Place("industrial/building-c",{9,48,0},4,Pi,true);
    Place("industrial/water-tower",{29,33,0},4,0,true);
    Place("industrial/windmill-low",{10,37,0},4);
    for(int i=0;i<5;++i) Place("industrial/chimney-medium",{12.f+i*3.2f,40,0},2,0,true);
    for(int i=0;i<6;++i) Place("industrial/solar-panel-landscape",{10.f+i*3,49,0},3);
    for(int i=0;i<7;++i) Place("industrial/shipping-container-a",{8.f+i*3.3f,33,0},3,0,true);
    for(int i=0;i<7;++i) Place("building/column",{9.f+i*3.1f,28,0},2,0,true);
    for(int i=0;i<7;++i) Cube({9.f+i*3.1f,28,4.9f},{.20f,9,.22f},gold);
    Cube({18,28,4.9f},{20,.22f,.22f},gold);

    DirectionalLight sun;sun.direction={-.65f,-.45f,1};sun.intensity=2.5f;sun.shadowStrength=.88f;
    Require(bool(scene.Add(sun)),"Sun");
    for(int i=0;i<2;++i) {
        SpotLight light;light.position={12.f+i*12,31,6.5f};light.direction={0,1,-.8f};light.range=25;
        light.outerConeRadians=.85f;light.innerConeRadians=.42f;light.intensity=30;
        light.color=i?float3{.14f,.50f,1}:float3{1,.47f,.12f};light.castShadow=true;
        Require(bool(scene.Add(light)),"Spotlight");
    }
    for(float y:{13.f,21.f,25.f}) {
        PointLight lamp;lamp.position={-18,y,3.1f};lamp.range=8;lamp.intensity=8;lamp.castShadow=false;lamp.color={1,.75f,.38f};
        Require(bool(scene.Add(lamp)),"Interior lamp");
    }
    LoadCharacter();
    for(const auto& station:Stations)Require(CanStand(station.position,solids),std::string("Blocked comparison pad: ")+station.name);
    Controller entry;entry.position={-18,6.5f};
    for(int i=0;i<150;++i)entry.Move(0,1,false,1.f/60,solids);
    Require(entry.position.y>13,"World doorway/corridor is blocked");
    std::printf("WORLD: %u entities, %zu attachments, %zu colliders, %zu external models\n",scene.GetEntityCount(),attachmentCount,solids.size(),models.size());
}

void World::LoadCharacter()
{
    Require(LoadModel((root/"Kenney/characters/Model/characterMedium.fbx").string(),character),"Load character");
    Require(!character.meshes.empty()&&!character.skins.empty(),"Character mesh/skin missing");
    Image skin;Require(LoadImage((root/"Kenney/characters/Skins/skaterMaleA.png").string(),skin),"Character texture");
    MaterialDesc material;material.baseColorTexture=skin;material.roughnessFactor=.9f;
    for(const auto& source:character.meshes) {
        Require(source.skinInfluences.size()==source.mesh.vertices.size(),"Character influence count");
        std::map<uint32_t,MeshData> parts;
        for(size_t i=0;i<source.mesh.indices.size();i+=3) {
            std::map<uint32_t,float> weights;
            for(int v=0;v<3;++v) {
                const auto& influence=source.skinInfluences[source.mesh.indices[i+v]];
                const float w[]={influence.weights.x,influence.weights.y,influence.weights.z,influence.weights.w};
                for(int j=0;j<4;++j) weights[influence.jointIndices[j]]+=w[j];
            }
            const auto joint=std::max_element(weights.begin(),weights.end(),[](auto& a,auto& b){return a.second<b.second;})->first;
            auto& part=parts[joint];
            for(int v=0;v<3;++v) {
                const auto vertex=source.mesh.vertices[source.mesh.indices[i+v]];
                part.indices.push_back(uint32_t(part.vertices.size()));part.vertices.push_back(vertex);
            }
        }
        for(auto& [joint,part]:parts) {
            auto entity=scene.Add(part,material);Require(bool(entity),"Character body part");
            body.push_back({entity,joint,scene.GetEntityMesh(static_cast<EntityID>(scene.GetEntityCount()-1))});
        }
    }
    const auto merged=MergeModelMeshes(character);
    float3 bindLow={1e9f,1e9f,1e9f},bindHigh={-1e9f,-1e9f,-1e9f};
    for(const auto& vertex:merged.vertices) {
        const auto p=vertex.position;
        bindLow={std::min(bindLow.x,p.x),std::min(bindLow.y,p.y),std::min(bindLow.z,p.z)};
        bindHigh={std::max(bindHigh.x,p.x),std::max(bindHigh.y,p.y),std::max(bindHigh.z,p.z)};
    }
    // FBX mesh-local Z is up, but its authored node rotation/scaling makes world Y up in centimetres.
    characterBottom=bindLow.y;characterScale=1.75f/(bindHigh.y-bindLow.y);
    Require(std::isfinite(characterScale)&&characterScale>0,"Character bounds invalid");
    runClip=LoadClip(root/"Kenney/characters/Animations/run.fbx",character.nodes);
    idleClip=LoadClip(root/"Kenney/characters/Animations/idle.fbx",character.nodes);
    pose.resize(character.nodes.size());globals.resize(character.nodes.size());
    std::printf("CHARACTER: %zu rigid joint parts, bind height %.3f -> 1.75m, run %.2fs (%zu tracks), idle %.2fs\n",body.size(),bindHigh.y-bindLow.y,runClip.duration,runClip.tracks.size(),idleClip.duration);
    Controller player;player.position=Stations[0].position;AnimateCharacter(player,0);
    float3 lowWorld={1e9f,1e9f,1e9f},highWorld={-1e9f,-1e9f,-1e9f};
    for(const auto& part:body) {
        const auto transform=part.entity.GetTransform();
        const auto& mesh=*scene.Meshes()[ToIndex(part.mesh)];
        for(const auto& vertex:mesh.vertices) {
            const auto p=TransformPoint(transform,vertex.position);
            lowWorld={std::min(lowWorld.x,p.x),std::min(lowWorld.y,p.y),std::min(lowWorld.z,p.z)};
            highWorld={std::max(highWorld.x,p.x),std::max(highWorld.y,p.y),std::max(highWorld.z,p.z)};
        }
    }
    Require(highWorld.z-lowWorld.z>1.5f&&highWorld.z-lowWorld.z<2.f&&std::abs(lowWorld.z)<.15f,"Character pose must remain grounded and human-sized");
    std::printf("CHARACTER posed world bounds: (%.3f %.3f %.3f) to (%.3f %.3f %.3f)\n",lowWorld.x,lowWorld.y,lowWorld.z,highWorld.x,highWorld.y,highWorld.z);
}

void World::AnimateCharacter(const Controller& player,float dt)
{
    animationTime+=std::clamp(dt,0.f,.05f)*(player.sprinting?1.5f:1.f);
    const auto& clip=player.speed>.1f?runClip:idleClip;
    for(size_t i=0;i<pose.size();++i) pose[i]=character.nodes[i].bindTransform;
    Require(SampleAnimationClip(clip,std::fmod(animationTime,std::max(.01f,clip.duration)),pose),"Sample character animation");
    Require(BuildGlobalNodeMatrices(character.nodes,pose,globals),"Character hierarchy");
    const auto rootTransform=Translation({player.position.x,player.position.y,0})*RotationZ(-player.facing+Pi)*Scaling(characterScale)*RotationX(Pi*.5f)*Translation({0,-characterBottom,0});
    const auto& skeleton=character.skins.front();
    // shortcut: rigid dominant-joint triangles retain stock instancing/culling; use ModelRenderer for smooth GPU skinning comparisons.
    for(auto& part:body) {
        Require(part.joint<skeleton.jointNodeIndices.size(),"Character joint out of range");
        const auto node=skeleton.jointNodeIndices[part.joint];
        const auto transform=rootTransform*globals[node]*skeleton.inverseBindMatrices[part.joint];
        Require(part.entity.SetTransform(transform),"Animate character part");
    }
}
}
