cbuffer TransformData : register(b0)
{
    float4 offset;
    uint textureIndex;
    uint vertexBufferIndex;
    uint indexBufferIndex;
    uint pad1;
};

Texture2D GlobalTextures[] : register(t0, space0);
SamplerState LinearSampler : register(s0);

struct PixelInput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD;
    float3 Color : COLOR;
};

float4 main(PixelInput input) : SV_TARGET
{
    // Bindless texture sampling
    float4 texColor = GlobalTextures[textureIndex].Sample(LinearSampler, input.UV);
    return texColor * float4(input.Color, 1.0f);
}
