Texture2D<float4> sourceImage : register(t0);
SamplerState imageSampler : register(s1);
struct VertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };

float4 main(VertexOutput input) : SV_Target
{
    return sourceImage.Sample(imageSampler, input.uv);
}
