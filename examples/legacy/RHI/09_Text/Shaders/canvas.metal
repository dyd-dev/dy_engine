#include <metal_stdlib>
using namespace metal;
struct Input { float2 position [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 color [[attribute(2)]]; };
struct Output { float4 position [[position]]; float2 uv; float4 color; };
// 이미지는 선형으로 샘플링하고 Canvas의 화면 색상 공간에서 tint를 곱한다.
float ToSrgb(float value)
{
    return value<=0.0031308f ? value*12.92f : 1.055f*pow(value,1.0f/2.4f)-0.055f;
}

vertex Output canvasVertex(Input input [[stage_in]],constant float4& transform [[buffer(15)]])
{
    return {float4(input.position*transform.xy+transform.zw,0,1),input.uv,input.color};
}
fragment float4 canvasFragment(Output input [[stage_in]],texture2d<float> image [[texture(0)]],
    sampler imageSampler [[sampler(1)]])
{
    float4 sampled=image.sample(imageSampler,input.uv);
    sampled.rgb=float3(ToSrgb(sampled.r),ToSrgb(sampled.g),ToSrgb(sampled.b));
    float4 result=input.color*sampled;
    return result;
}
