Texture2D<float4> sourceImage:register(t0);
SamplerState sourceSampler:register(s1);
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target0
{
    return sourceImage.SampleLevel(sourceSampler,uv,0);
}
