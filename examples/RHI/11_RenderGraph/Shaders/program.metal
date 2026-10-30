#include <metal_stdlib>
using namespace metal;
struct GeneratedVertex { float4 position; float4 color; };
kernel void computeMain(device GeneratedVertex* outputVertices [[buffer(0)]],constant float4& parameters [[buffer(15)]],
    uint id [[thread_position_in_grid]]) {
    if(id>=3) return;
    const float2 positions[3]={float2(-0.7,-0.6),float2(0.7,-0.6),float2(0,0.7)};
    const float3 colors[3]={float3(1,0.2,0.15),float3(0.2,1,0.3),float3(0.2,0.4,1)};
    float angle=parameters.x*0.4f,c=cos(angle),s=sin(angle); float2 p=positions[id];
    outputVertices[id].position=float4(c*p.x-s*p.y,s*p.x+c*p.y,0,1);
    outputVertices[id].color=float4(colors[id]*(0.75f+0.25f*sin(parameters.x+float(id))),1);
}
struct VertexInput { float4 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
struct VertexOutput { float4 position [[position]]; float4 color; };
vertex VertexOutput vertexMain(VertexInput input [[stage_in]]) { return {input.position,input.color}; }
fragment float4 fragmentMain(VertexOutput input [[stage_in]]) { return input.color; }

struct DisplayOutput { float4 position [[position]]; float2 uv; };
vertex DisplayOutput displayVertex(uint id [[vertex_id]]) {
    const float2 positions[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    return {float4(positions[id],0,1),positions[id]*float2(0.5,-0.5)+0.5f};
}
fragment float4 displayFragment(DisplayOutput input [[stage_in]],texture2d<float> image [[texture(0)]],sampler imageSampler [[sampler(1)]]) {
    return image.sample(imageSampler,input.uv);
}
