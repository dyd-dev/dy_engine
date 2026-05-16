#include <metal_stdlib>
using namespace metal;

struct VSOutput
{
    float4 position [[position]];
    float2 uv;
};

fragment float4 main0(
    VSOutput input [[stage_in]])
{
    return float4(input.uv, 0.0, 1.0);
}
