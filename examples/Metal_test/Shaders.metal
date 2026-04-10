#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 position;
    float2 texCoord;
    float4 color;
};

struct VertexOut {
    float4 position [[position]];
    float2 texCoord;
    float4 color;
};

vertex VertexOut vertexShader(uint vertexID [[vertex_id]],
                              constant VertexIn* vertices [[buffer(0)]])
{
    VertexOut out;
    out.position = float4(vertices[vertexID].position, 0.0, 1.0);
    out.texCoord = vertices[vertexID].texCoord;
    out.color    = vertices[vertexID].color;
    return out;
}

fragment float4 fragmentShader(VertexOut in [[stage_in]],
                                texture2d<float> tex [[texture(0)]])
{
    constexpr sampler s(filter::linear);
    if(is_null_texture(tex))
        return in.color;
    return tex.sample(s, in.texCoord);
}
