#include <metal_stdlib>
using namespace metal;
struct Output { float4 position [[position]]; float4 color; };
vertex Output vertexMain(uint id [[vertex_id]]) {
    const float2 positions[3] = {float2(-0.7,-0.6),float2(0.7,-0.6),float2(0,0.7)};
    const float4 colors[3] = {float4(1,0.2,0.15,1),float4(0.2,1,0.3,1),float4(0.2,0.4,1,1)};
    return {float4(positions[id],0,1),colors[id]};
}
fragment float4 fragmentMain(Output input [[stage_in]]) { return input.color; }

struct DisplayOutput { float4 position [[position]]; float2 uv; };
vertex DisplayOutput displayVertex(uint id [[vertex_id]]) {
    const float2 positions[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    return {float4(positions[id],0,1),positions[id]*float2(0.5,-0.5)+0.5f};
}
fragment float4 displayFragment(DisplayOutput input [[stage_in]],texture2d<float> image [[texture(0)]],sampler imageSampler [[sampler(1)]]) {
    return image.sample(imageSampler,input.uv);
}
