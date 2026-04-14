struct DrawConstants
{
    float4x4 worldMatrix;
    float4 baseColor;
    uint baseColorTextureIndex;
    float3 padding;
};

ConstantBuffer<DrawConstants> gDraw : register(b0);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    const float2 positions[3] =
    {
        float2(0.0f, 0.6f),
        float2(0.6f, -0.6f),
        float2(-0.6f, -0.6f)
    };

    const float2 uvs[3] =
    {
        float2(0.5f, 0.0f),
        float2(1.0f, 1.0f),
        float2(0.0f, 1.0f)
    };

    VSOutput output;
    output.position = mul(gDraw.worldMatrix, float4(positions[vertexID], 0.0f, 1.0f));
    output.uv = uvs[vertexID];
    return output;
}
