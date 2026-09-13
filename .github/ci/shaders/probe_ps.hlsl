cbuffer Constants : register(b0) { float4 color; float4 parameters; };
Texture2D colorTexture : register(t0);
SamplerState colorSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    return parameters.w > 0.5 ? colorTexture.Sample(colorSampler, uv) : color;
}
