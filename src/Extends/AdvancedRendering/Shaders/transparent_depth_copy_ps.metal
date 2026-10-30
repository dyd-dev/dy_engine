#include <metal_stdlib>
using namespace metal;
struct TransparentDepthCopyVertex {float4 position [[position]];float2 uv;};
fragment float transparentDepthCopyFragment(TransparentDepthCopyVertex input [[stage_in]],
    depth2d<float> sourceDepth [[texture(5)]])
{
    return sourceDepth.read(uint2(input.position.xy));
}
