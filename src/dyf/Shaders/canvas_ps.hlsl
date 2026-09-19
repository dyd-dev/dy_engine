Texture2D image:register(t0);
SamplerState imageSampler:register(s1);
cbuffer Transform:register(b15) { float4 transform; float4 settings; };
// 이미지는 선형으로 샘플링하고 Canvas의 화면 색상 공간에서 tint를 곱한다.
float ToSrgb(float value)
{
    return value<=0.0031308 ? value*12.92 : 1.055*pow(value,1.0/2.4)-0.055;
}
float ToLinear(float value)
{
    return value<=0.04045 ? value/12.92 : pow((value+0.055)/1.055,2.4);
}

float4 main(float4 position:SV_POSITION,float2 uv:TEXCOORD0,float4 color:TEXCOORD1):SV_TARGET
{
    float4 sampled=image.Sample(imageSampler,uv);
    sampled.rgb=float3(ToSrgb(sampled.r),ToSrgb(sampled.g),ToSrgb(sampled.b));
    float4 result=color*sampled;
    if(settings.x>0.5)
        result.rgb=float3(ToLinear(result.r),ToLinear(result.g),ToLinear(result.b));
    return result;
}
