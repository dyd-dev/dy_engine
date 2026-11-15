#include <metal_stdlib>
using namespace metal;
vertex float4 surfaceOwnerProbeVertex(uint vertex [[vertex_id]])
{
    float2 p=float2((vertex<<1)&2,vertex&2);
    return float4(p*2.0f-1.0f,0,1);
}
fragment float4 surfaceOwnerProbeFragment(float4 position [[position]],texture2d<uint> ownerTexture [[texture(0)]])
{
    uint2 owner=ownerTexture.read(uint2(position.xy)).rg;
    return float4(float2(owner)/255.0f,0,1);
}
