#include <metal_stdlib>
using namespace metal;
struct Input { float2 position [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 color [[attribute(2)]]; };
struct Output { float4 position [[position]]; float2 uv; float4 color; };
struct Transform { float4 value; float4 settings; };
// 이미지는 선형으로 샘플링하고 Canvas의 화면 색상 공간에서 tint를 곱한다.
float ToSrgb(float value)
{
    return value<=0.0031308f ? value*12.92f : 1.055f*pow(value,1.0f/2.4f)-0.055f;
}
float ToLinear(float value)
{
    return value<=0.04045f ? value/12.92f : pow((value+0.055f)/1.055f,2.4f);
}

vertex Output canvasVertex(Input input [[stage_in]],constant Transform& transform [[buffer(15)]])
{
    return {float4(input.position*transform.value.xy+transform.value.zw,0,1),input.uv,input.color};
}
fragment float4 canvasFragment(Output input [[stage_in]],texture2d<float> image [[texture(0)]],
    sampler imageSampler [[sampler(1)]],constant Transform& transform [[buffer(15)]])
{
    float4 sampled=image.sample(imageSampler,input.uv);
    sampled.rgb=float3(ToSrgb(sampled.r),ToSrgb(sampled.g),ToSrgb(sampled.b));
    float4 result=input.color*sampled;
    if(transform.settings.x>0.5f)
        result.rgb=float3(ToLinear(result.r),ToLinear(result.g),ToLinear(result.b));
    return result;
}
