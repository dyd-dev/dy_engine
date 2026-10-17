#include <metal_stdlib>
using namespace metal;
struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};
struct ShadowOutput {float4 position [[position]]; float2 uv;};
fragment void shadowFragmentShader(
    ShadowOutput input [[stage_in]],
    bool frontFace [[front_facing]],
    constant DrawConstants& constants [[buffer(10)]],
    texture2d<float> baseColorTexture [[texture(0)]],
    sampler materialSampler [[sampler(8)]])
{
    if((constants.padding1 & 4u)==0u && !frontFace) discard_fragment();
    if((constants.padding1 & 3u)==1u)
    {
        float alpha=constants.baseColor.a;
        if((constants.textureFlags & 1u)!=0u) alpha*=baseColorTexture.sample(materialSampler,input.uv).a;
        if(alpha<as_type<float>(constants.padding0)) discard_fragment();
    }
}
