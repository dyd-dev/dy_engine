#include <metal_stdlib>
using namespace metal;
struct PresentationVertex
{
    float4 position [[position]];
    float2 uv;
};
vertex PresentationVertex presentationVertex(uint id [[vertex_id]])
{
    PresentationVertex output;
    output.uv=float2((id<<1)&2,id&2);
    output.position=float4(output.uv*float2(2,-2)+float2(-1,1),0,1);
    return output;
}
fragment float4 presentationFragment(PresentationVertex input [[stage_in]],
    texture2d<float> sourceImage [[texture(0)]],sampler sourceSampler [[sampler(1)]])
{
    return sourceImage.sample(sourceSampler,input.uv,level(0));
}
