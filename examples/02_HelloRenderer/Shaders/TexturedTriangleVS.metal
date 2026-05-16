#include <metal_stdlib>
using namespace metal;

struct VSOutput
{
    float4 position [[position]];
    float2 uv;
};

struct VSInput
{
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
};

vertex VSOutput main0(VSInput input [[stage_in]])
{
    VSOutput output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    return output;
}
