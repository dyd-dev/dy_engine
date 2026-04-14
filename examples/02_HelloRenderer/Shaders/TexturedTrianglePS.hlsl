struct DrawConstants
{
    float4x4 worldMatrix;
    float4 baseColor;
    uint baseColorTextureIndex;
    float3 padding;
};

ConstantBuffer<DrawConstants> gDraw : register(b0);
Texture2D gTextures[] : register(t0, space0);
SamplerState gLinearSampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    return gTextures[gDraw.baseColorTextureIndex].Sample(gLinearSampler, uv) * gDraw.baseColor;
}
