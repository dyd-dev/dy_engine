#include <metal_stdlib>
using namespace metal;
struct TransparentCopyVertex {float4 position [[position]];float2 uv;};
fragment float4 transparentCopyFragment(TransparentCopyVertex input [[stage_in]],texture2d<float> opaqueTexture [[texture(5)]],sampler screenSampler [[sampler(9)]])
{
    return float4(opaqueTexture.sample(screenSampler,input.uv,level(0)).rgb,1);
}
