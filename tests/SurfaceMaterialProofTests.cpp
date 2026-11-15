#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/dyf/DepthPrepassValidation.h"
#include <cassert>

int main()
{
    using namespace dyf;
    static_assert(SurfaceOwnerStride(0)==0 && SurfaceOwnerStride(0xffffffu)==0xffffffu && SurfaceOwnerStride(0x1000000u)==0);
    static_assert(SurfaceOwnerIndicesFit(2479,19391904)); // Emerald requires 33 bits with fixed bit fields.
    static_assert(SurfaceOwnerIndicesFit(65535,65537*12) && !SurfaceOwnerIndicesFit(65535,65538*12));
    static_assert(!SurfaceOwnerIndicesFit(0,12));
    MaterialDesc material;
    RenderGeometryDraw draw;draw.material=&material;
    std::vector<RenderGeometryDraw> draws{draw};
    SurfaceGeometrySnapshot proof;proof.Capture(draws);
    assert(proof.Matches(draws));
    const auto changed=[&](auto& field,const auto value) {
        const auto previous=field;field=value;
        assert(!proof.Matches(draws));field=previous;
        assert(proof.Matches(draws));
    };
    changed(material.baseColor.x,.2f);
    changed(material.emissiveColor.y,.3f);
    changed(material.metallicFactor,.6f);
    changed(material.roughnessFactor,.1f);
    changed(material.normalScale,.4f);
    changed(material.normalYSign,-1.f);
    changed(material.occlusionStrength,.2f);
    changed(material.unlit,true);
    changed(material.doubleSided,true);
    changed(material.alphaMode,MaterialAlphaMode::Mask);
    changed(material.alphaCutoff,.3f);
    changed(draws[0].baseColor.z,.7f);
    changed(draws[0].indirectCount,3u);
    changed(draws[0].indirectOffset,20u);
    changed(draws[0].model.m[4],.25f);
    changed(draws[0].textures[0],reinterpret_cast<RHI::TextureHandle>(uintptr_t{8}));
    material.baseColorTexture.SetColorSpace(ColorSpace::Linear);
    assert(!proof.Matches(draws));
    material.baseColorTexture.SetColorSpace(ColorSpace::Srgb);
    assert(proof.Matches(draws));
    draws.push_back(draw);
    assert(!proof.Matches(draws));
}
