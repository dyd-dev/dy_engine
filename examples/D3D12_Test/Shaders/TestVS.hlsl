cbuffer TransformData : register(b0)
{
    float4 offset;
    uint textureIndex;
    uint vertexBufferIndex;
    uint indexBufferIndex;
    uint pad1;
};

struct VertexInput
{
    float3 Pos;
    float2 UV;
    float3 Color;
};

struct PixelInput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD;
    float3 Color : COLOR;
};

StructuredBuffer<VertexInput> g_VertexBuffers[] : register(t0, space1);
StructuredBuffer<uint> g_IndexBuffers[] : register(t0, space2);

PixelInput main(uint vertexID : SV_VertexID)
{
    PixelInput output;

    uint realIndex = g_IndexBuffers[indexBufferIndex][vertexID];
    VertexInput input = g_VertexBuffers[vertexBufferIndex][realIndex];

    float3 finalPos = (input.Pos * 0.05f) + offset.xyz;

    output.Pos = float4(finalPos, 1.0f);
    output.UV = input.UV;
    output.Color = input.Color;

    return output;
}
