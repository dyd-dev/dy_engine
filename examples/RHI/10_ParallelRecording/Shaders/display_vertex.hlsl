cbuffer Transform : register(b15) { float4 transform; };
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };

VertexOutput main(uint vertexID : SV_VertexID)
{
    const float2 positions[6] = {
        float2(-1, -1), float2(1, -1), float2(1, 1),
        float2(1, 1), float2(-1, 1), float2(-1, -1)};
    float2 p = positions[vertexID];
    VertexOutput output;
    output.position = float4(p * transform.zw + transform.xy, 0, 1);
    output.uv = p * float2(0.5, -0.5) + 0.5;
    return output;
}
