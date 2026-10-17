cbuffer Parameters : register(b15) { float4 parameters; };
struct VertexOutput { float4 position : SV_Position; float4 color : COLOR0; };

VertexOutput main(uint vertexID : SV_VertexID)
{
    const float2 positions[3] = {float2(-0.7, -0.6), float2(0.7, -0.6), float2(0, 0.7)};
    const float3 colors[3] = {float3(1, 0.2, 0.15), float3(0.2, 1, 0.3), float3(0.2, 0.4, 1)};
    float2 p = positions[vertexID];
    float c = cos(parameters.x), s = sin(parameters.x);
    VertexOutput output;
    output.position = float4(c * p.x - s * p.y, s * p.x + c * p.y, 0, 1);
    output.color = float4(colors[vertexID], 1);
    return output;
}
