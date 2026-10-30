#include <metal_stdlib>
using namespace metal;

kernel void generateArguments(constant float4& settings [[buffer(15)]], device uint* words [[buffer(0)]])
{
    uint count = uint(floor(64.0f * clamp(0.5f + 0.75f * sin(settings.x * 0.65f), 0.0f, 1.0f)));
    words[0] = 3; words[1] = count; words[2] = 0; words[3] = 0;
    words[4] = 3; words[5] = 64 - count; words[6] = 0; words[7] = 0; words[8] = 0;
}

struct Output { float4 position [[position]]; float3 color; };
vertex Output vertexMain(uint vertex [[vertex_id]], uint instance [[instance_id]],
    constant float4& display [[buffer(15)]])
{
    const float2 vertices[3] = {float2(-0.075f, -0.065f), float2(0.075f, -0.065f), float2(0.0f, 0.08f)};
    float2 center = float2((float(instance % 8) - 3.5f) * 0.18f + mix(-0.78f, 0.78f, display.z),
        (float(instance / 8) - 3.5f) * 0.20f);
    float3 color = mix(float3(0.12f, 0.80f, 0.95f), float3(1.0f, 0.46f, 0.12f), display.z)
        * (0.65f + 0.35f * float(instance % 8) / 7.0f);
    return {float4((center + vertices[vertex]) * display.xy, 0.0f, 1.0f), color};
}
fragment float4 fragmentMain(Output input [[stage_in]]) { return float4(input.color, 1.0f); }
