#include <metal_stdlib>
using namespace metal;

struct SceneOutput { float4 position [[position]]; float4 color; };
struct DisplayOutput { float4 position [[position]]; float2 uv; };

vertex SceneOutput sceneVertex(uint vertexID [[vertex_id]],
    constant float4& parameters [[buffer(15)]])
{
    const float2 positions[3] = {float2(-0.7, -0.6), float2(0.7, -0.6), float2(0, 0.7)};
    const float3 colors[3] = {float3(1, 0.2, 0.15), float3(0.2, 1, 0.3), float3(0.2, 0.4, 1)};
    float2 p = positions[vertexID];
    float c = cos(parameters.x), s = sin(parameters.x);
    return {float4(c * p.x - s * p.y, s * p.x + c * p.y, 0, 1), float4(colors[vertexID], 1)};
}

fragment float4 sceneFragment(SceneOutput input [[stage_in]])
{
    return input.color;
}

vertex DisplayOutput displayVertex(uint vertexID [[vertex_id]],
    constant float4& transform [[buffer(15)]])
{
    const float2 positions[6] = {
        float2(-1, -1), float2(1, -1), float2(1, 1),
        float2(1, 1), float2(-1, 1), float2(-1, -1)};
    float2 p = positions[vertexID];
    return {float4(p * transform.zw + transform.xy, 0, 1), p * float2(0.5, -0.5) + 0.5};
}

fragment float4 displayFragment(DisplayOutput input [[stage_in]],
    texture2d<float> sourceImage [[texture(0)]], sampler imageSampler [[sampler(1)]])
{
    return sourceImage.sample(imageSampler, input.uv);
}
