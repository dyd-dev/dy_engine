Texture2D<float4> materialTextures[4] : register(t0);
SamplerState materialSampler : register(s4);
cbuffer Constants : register(b15) { float4 transform; float4 parameters; };
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    return materialTextures[(uint)parameters.x].Sample(materialSampler, uv);
}
