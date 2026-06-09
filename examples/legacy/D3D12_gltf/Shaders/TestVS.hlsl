cbuffer TransformData : register(b0)
{
    float4 offset;
    uint textureIndex;
    uint vertexBufferIndex;
    uint indexBufferIndex;
    float rotationY; // 회전각 추가
};

struct VertexInput
{
    float3 Pos;
    float2 UV;
    float3 Color;
    float3 Normal; // 44바이트 스트라이드 매핑용 추가
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

    // Y축 회전 행렬 계산
    float s = sin(rotationY);
    float c = cos(rotationY);
    float3 rotatedPos = input.Pos;
    rotatedPos.x = input.Pos.x * c - input.Pos.z * s;
    rotatedPos.z = input.Pos.x * s + input.Pos.z * c;

    float3 finalPos = (rotatedPos * offset.w) + offset.xyz;

    output.Pos = float4(finalPos, 1.0f);
    output.UV = input.UV;
    output.Color = input.Color;

    return output;
}
