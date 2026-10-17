#include <metal_stdlib>
using namespace metal;
struct Output {float4 position [[position]];float2 uv;};
struct Settings {float4 reconstruction;};
vertex Output environmentDepthVertex(uint id [[vertex_id]])
{
    Output output;output.uv=float2((id<<1)&2,id&2);
    output.position=float4(output.uv*float2(2,-2)+float2(-1,1),0,1);return output;
}
fragment float4 environmentDepthFragment(Output input [[stage_in]],depth2d<float> sourceDepth [[texture(0)]],
    sampler depthSampler [[sampler(1)]],constant Settings& settings [[buffer(15)]])
{
    float depth=sourceDepth.sample(depthSampler,input.uv,level(0));
    float4 r=settings.reconstruction;
    float viewDistance=depth>=1 ? r.w : r.z/((1-depth)+depth*(r.z/r.w));
    float2 ray=(input.uv*float2(2,-2)+float2(-1,1))*r.xy;
    uint bits=as_type<uint>(viewDistance*length(float3(ray,1)));
    return float4(bits&255u,(bits>>8)&255u,(bits>>16)&255u,(bits>>24)&255u)/255.0;
}
