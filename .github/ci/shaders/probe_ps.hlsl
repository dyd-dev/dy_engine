cbuffer Constants : register(b15) { float4 color; float4 parameters; };
Texture2D colorTexture : register(t0);
SamplerState colorSampler : register(s1);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    return parameters.w > 0.5 ? colorTexture.Sample(colorSampler, uv) : color;
}
