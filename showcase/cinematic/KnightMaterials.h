#pragma once

#include "dyf/Extends/Model/Model.h"
#include <filesystem>

namespace showcase
{
// 번들의 Maya USD Preview Surface 값을 FBX에서 누락된 재질에 명시적으로 연결한다.
inline void ApplyIntelKnightMaterials(dyf::ModelSceneDesc& actor)
{
    using namespace dyf;
    const auto root=std::filesystem::path(actor.path).parent_path().parent_path().parent_path();
    const auto textures=root/"Textures";
    struct Surface
    {
        const char* name;
        Math::float4 color;
        float metallic,roughness;
        const char* baseColor;
        const char* normal;
        const char* roughnessMap;
    };
    const Surface surfaces[]={
        {"Armor",{.5f,.5f,.5f,1},1,1,nullptr,nullptr,"brushed_metal_rough_F.png"},
        {"Chestplate",{.5f,.5f,.5f,1},1,1,nullptr,nullptr,"brushed_metal_rough_D.png"},
        {"Shoulder",{.5f,.5f,.5f,1},1,1,nullptr,nullptr,"brushed_metal_rough_H.png"},
        {"Brass",{1,.69856846f,.37800002f,1},1,.25f,nullptr,nullptr,nullptr},
        {"Leather",{1,1,1,1},0,.6f,"leather_diffuse_D.png",nullptr,nullptr},
        {"Cloth",{1,1,1,1},0,.693f,"cloth_diffuse_D.png","cloth_normal_D.png",nullptr},
        {"Shield",{1,1,1,1},1,.25f,"shield_final_diffuse_A.png","shield_final_normal_A.png",nullptr},
        {"Sword",{1,1,1,1},1,1,"sword_diffuse_C.png",nullptr,"sword_rough_D.png"}
    };
    auto& options=actor.loadOptions;
    options.normalMapYSign=1;
    // 원본 재질·ORM 결합의 상한이다. 호출자가 별도로 지정한 텍스처 크기 제한은 보존한다.
    options.maxSourceBytes=options.maxParserInputBytes=options.maxParserBytes=
        options.maxDecodedBytes=8ull*1024*1024*1024;
    for(const auto& surface:surfaces)
    {
        ModelMaterialOverride value;
        value.name=std::string("knight_rig:usdPreviewSurface_")+surface.name;
        value.baseColor=surface.color;
        value.metallicFactor=surface.metallic;value.roughnessFactor=surface.roughness;
        if(surface.baseColor) value.texturePaths[ToIndex(MaterialTextureKind::BaseColor)]=(textures/surface.baseColor).string();
        if(surface.normal) value.texturePaths[ToIndex(MaterialTextureKind::Normal)]=(textures/surface.normal).string();
        if(surface.roughnessMap) value.roughnessTexturePath=(textures/surface.roughnessMap).string();
        options.materialOverrides.push_back(std::move(value));
    }
}
}
