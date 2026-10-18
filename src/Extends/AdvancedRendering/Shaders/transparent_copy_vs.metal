#include <metal_stdlib>
using namespace metal;
struct TransparentCopyVertex {float4 position [[position]];float2 uv;};
vertex TransparentCopyVertex transparentCopyVertex(uint id [[vertex_id]])
{
    TransparentCopyVertex output;output.uv=float2((id<<1)&2,id&2);
    output.position=float4(output.uv*float2(2,-2)+float2(-1,1),0,1);return output;
}
