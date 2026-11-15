#pragma once

#include "dyf/RenderExtension.h"
#include "dyf/Camera.h"
#include "ImageDds.h"
#include <array>
#include <cstring>
#include <vector>

namespace dyf
{
// Capture values, including material scalars, rather than retaining pointers to
// mutable material fields or comparing padding in the public draw structure.
struct DepthGeometryInput
{
    RHI::BufferHandle vertices=nullptr,indices=nullptr,instances=nullptr,arguments=nullptr;
    RHI::TextureHandle alphaTexture=nullptr;
    uint32_t instanceOffset=0,argumentOffset=0,indexCount=0,instanceCount=0,indirectCount=0,textureFlags=0;
    Math::float4x4 model;
    Math::Bounds3 bounds;
    const MaterialDesc* material=nullptr;
    MaterialAlphaMode alphaMode=MaterialAlphaMode::Opaque;
    float alpha=1,materialAlpha=1,cutoff=0;
    bool doubleSided=false;

    explicit DepthGeometryInput(const RenderGeometryDraw& draw)
        :vertices(draw.vertexBuffer),indices(draw.indexBuffer),instances(draw.instanceBuffer),
        arguments(draw.indirectArguments),alphaTexture(draw.textures[ToIndex(MaterialTextureKind::BaseColor)]),
        instanceOffset(draw.instanceOffset),argumentOffset(draw.indirectOffset),indexCount(draw.indexCount),
        instanceCount(draw.instanceCount),indirectCount(draw.indirectCount),textureFlags(draw.textureFlags),model(draw.model),bounds(draw.localBounds),
        material(draw.material),alpha(draw.baseColor.w)
    {
        if(material) {alphaMode=material->alphaMode;materialAlpha=material->baseColor.w;
            cutoff=material->alphaCutoff;doubleSided=material->doubleSided;}
    }
    bool Matches(const RenderGeometryDraw& draw) const
    {
        const DepthGeometryInput current(draw);
        return vertices==current.vertices && indices==current.indices && instances==current.instances &&
            arguments==current.arguments && alphaTexture==current.alphaTexture &&
            instanceOffset==current.instanceOffset && argumentOffset==current.argumentOffset &&
            indexCount==current.indexCount && instanceCount==current.instanceCount && indirectCount==current.indirectCount && textureFlags==current.textureFlags &&
            std::memcmp(model.m,current.model.m,sizeof(model.m))==0 && bounds.valid==current.bounds.valid &&
            bounds.min.x==current.bounds.min.x && bounds.min.y==current.bounds.min.y && bounds.min.z==current.bounds.min.z &&
            bounds.max.x==current.bounds.max.x && bounds.max.y==current.bounds.max.y && bounds.max.z==current.bounds.max.z &&
            material==current.material && alphaMode==current.alphaMode && doubleSided==current.doubleSided &&
            std::memcmp(&alpha,&current.alpha,sizeof(alpha))==0 &&
            std::memcmp(&materialAlpha,&current.materialAlpha,sizeof(materialAlpha))==0 &&
            std::memcmp(&cutoff,&current.cutoff,sizeof(cutoff))==0;
    }
};
struct DepthGeometrySnapshot
{
    std::vector<DepthGeometryInput> inputs;
    void Capture(const std::vector<RenderGeometryDraw>& draws)
    {
        inputs.clear();inputs.reserve(draws.size());
        for(const auto& draw:draws)inputs.emplace_back(draw);
    }
    bool Matches(const std::vector<RenderGeometryDraw>& draws) const
    {
        if(draws.size()!=inputs.size())return false;
        for(size_t i=0;i<draws.size();++i)if(!inputs[i].Matches(draws[i]))return false;
        return true;
    }
};
// Immutable source identities plus every material scalar consumed by stock shading.
struct SurfaceGeometryInput
{
    DepthGeometryInput depth;
    std::array<RHI::TextureHandle,ToIndex(MaterialTextureKind::Count)> textures;
    std::array<const void*,5> images={};
    std::array<uint32_t,15> imageProperties={};
    std::array<float,24> values={};
    bool unlit=false;
    explicit SurfaceGeometryInput(const RenderGeometryDraw& draw):depth(draw),textures(draw.textures)
    {
        if(!draw.material)return;
        const auto& m=*draw.material;
        values={draw.baseColor.x,draw.baseColor.y,draw.baseColor.z,draw.baseColor.w,
            m.baseColor.x,m.baseColor.y,m.baseColor.z,m.baseColor.w,
            m.emissiveColor.x,m.emissiveColor.y,m.emissiveColor.z,m.metallicFactor,m.roughnessFactor,
            m.normalScale,m.normalYSign,m.occlusionStrength,m.transmissionFactor,m.indexOfRefraction,
            m.thickness,m.attenuationColor.x,m.attenuationColor.y,m.attenuationColor.z,m.attenuationDistance,m.alphaCutoff};
        unlit=m.unlit;
        const Image* sources[]={&m.baseColorTexture,&m.metallicRoughnessTexture,&m.normalTexture,&m.occlusionTexture,&m.emissiveTexture};
        for(uint32_t i=0;i<5;++i){images[i]=Detail::ImageDdsAccess::Identity(*sources[i]);
            imageProperties[i*3]=sources[i]->GetWidth();imageProperties[i*3+1]=sources[i]->GetHeight();
            imageProperties[i*3+2]=static_cast<uint32_t>(sources[i]->GetColorSpace());}
    }
    bool Matches(const RenderGeometryDraw& draw) const
    {
        const SurfaceGeometryInput current(draw);
        return depth.Matches(draw) && textures==current.textures && images==current.images &&
            imageProperties==current.imageProperties && unlit==current.unlit &&
            std::memcmp(values.data(),current.values.data(),sizeof(values))==0;
    }
};
struct SurfaceGeometrySnapshot
{
    std::vector<SurfaceGeometryInput> inputs;
    void Capture(const std::vector<RenderGeometryDraw>& draws)
    {inputs.clear();inputs.reserve(draws.size());for(const auto& draw:draws)inputs.emplace_back(draw);}
    bool Matches(const std::vector<RenderGeometryDraw>& draws) const
    {if(inputs.size()!=draws.size())return false;for(size_t i=0;i<draws.size();++i)if(!inputs[i].Matches(draws[i]))return false;return true;}
};
inline bool SameDepthCamera(const Math::float4x4& view,const Math::float4x4& projection,const Camera& camera)
{
    return std::memcmp(view.m,camera.view.m,sizeof(view.m))==0 &&
        std::memcmp(projection.m,camera.projection.m,sizeof(projection.m))==0;
}
}
