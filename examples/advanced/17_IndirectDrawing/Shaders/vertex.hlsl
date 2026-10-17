cbuffer Display : register(b15) { float4 display; };
struct Output { float4 position : SV_Position; float3 color : COLOR0; };
Output main(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    const float2 vertices[3] = {float2(-0.075, -0.065), float2(0.075, -0.065), float2(0.0, 0.08)};
    float2 center = float2((float(instance % 8) - 3.5) * 0.18 + lerp(-0.78, 0.78, display.z),
        (float(instance / 8) - 3.5) * 0.20);
    Output output;
    output.position = float4((center + vertices[vertex]) * display.xy, 0.0, 1.0);
    output.color = lerp(float3(0.12, 0.80, 0.95), float3(1.0, 0.46, 0.12), display.z)
        * (0.65 + 0.35 * float(instance % 8) / 7.0);
    return output;
}
