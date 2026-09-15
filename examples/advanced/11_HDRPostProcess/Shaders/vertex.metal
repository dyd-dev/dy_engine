#include <metal_stdlib>
using namespace metal;
struct Output { float4 position [[position]]; float2 uv; };
vertex Output vertexMain(uint id [[vertex_id]]) {
    Output o; o.uv=float2((id<<1)&2,id&2);
    o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o;
}
