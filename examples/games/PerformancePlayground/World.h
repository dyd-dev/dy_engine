#pragma once
#include "Controller.h"
#include "dyf.h"
#include <dyf/Extends/Model.h>
#include <filesystem>
#include <map>
#include <string>

namespace playground {
struct Station {const char* name;const char* detail;Point position;float yaw,pitch;};
inline constexpr Station Stations[]={
    {"01  BASELINE PLAZA","Open space / a few buildings",{0,-20},3.14159265f,.23f},
    {"02  ATTACHMENT ALLEY","Repeated pipes, awnings and facade details",{18,-2},0,.22f},
    {"03  INTERIOR LAB","Walk through doors / walls still submit draws",{-18,16},0,.12f},
    {"04  LIGHT & SHADOW","Directional sun + warm / cool spotlights",{17,30},0,.22f}
};

class World {
public:
    explicit World(const std::filesystem::path& assets);
    dyf::ModelScene scene;
    std::vector<Box> solids;
    void AnimateCharacter(const Controller&,float seconds);
    size_t AttachmentCount() const {return attachmentCount;}
private:
    dyf::EntityHandle Cube(dyf::Math::float3 position,dyf::Math::float3 scale,dyf::Math::float4 color,bool solid=false);
    void Place(const std::string& asset,dyf::Math::float3 position,float scale=1,float angle=0,bool solid=false);
    void Wall(float x,float y,float length,float angle,bool window=false);
    void LoadCharacter();
    std::filesystem::path root;
    std::map<std::string,dyf::ModelAssetID> models;
    std::map<std::array<float,4>,dyf::EntityHandle> cubes;
    struct BodyPart {dyf::EntityHandle entity;uint32_t joint;dyf::MeshID mesh;};
    std::vector<BodyPart> body;
    dyf::ModelData character;
    dyf::AnimationClip runClip,idleClip;
    std::vector<dyf::NodeTransform> pose;
    std::vector<dyf::Math::float4x4> globals;
    float characterScale=1,characterBottom=0,animationTime=0;
    size_t attachmentCount=0;
};
}
