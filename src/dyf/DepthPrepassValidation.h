#pragma once

#include "dyf/RenderExtension.h"
#include "dyf/Camera.h"
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
    uint32_t instanceOffset=0,argumentOffset=0,indexCount=0,instanceCount=0,textureFlags=0;
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
        instanceCount(draw.instanceCount),textureFlags(draw.textureFlags),model(draw.model),bounds(draw.localBounds),
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
            indexCount==current.indexCount && instanceCount==current.instanceCount && textureFlags==current.textureFlags &&
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
inline bool SameDepthCamera(const Math::float4x4& view,const Math::float4x4& projection,const Camera& camera)
{
    return std::memcmp(view.m,camera.view.m,sizeof(view.m))==0 &&
        std::memcmp(projection.m,camera.projection.m,sizeof(projection.m))==0;
}
}
