Texture2D<float4> materialTexture : register(t0);
SamplerState materialSampler : register(s4);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    return materialTexture.Sample(materialSampler, uv);
}
