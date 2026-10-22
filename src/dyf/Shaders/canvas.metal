#include <metal_stdlib>
using namespace metal;

// CPU가 만든 Canvas 정점의 픽셀 위치·이미지 UV·곱할 색을 받는 입력이다.
struct Input { float2 position [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 color [[attribute(2)]]; };

// 투영 후 잘라내기 전 좌표 clip과 UV·곱할 색을 담아 canvasVertex에서 canvasFragment로 넘긴다. UV와 색은 삼각형 안의 픽셀 위치에 맞게 정점 사이의 값이 섞인다.
struct Output { float4 position [[position]]; float2 uv; float4 color; };

// value에는 픽셀 위치를 투영 후 좌표로 옮길 배율·이동값을 담는다. settings.x는 저장할 때 GPU가 표시용 색으로 변환하는 sRGB 출력인지 나타낸다.
struct Transform { float4 value; float4 settings; };

// 선형 RGB의 한 성분을 Canvas 지정 색과 같은 sRGB 값으로 바꾼다. 호출부가 RGB에만 적용하므로 알파는 유지된다.
float ToSrgb(float value)
{
    // 밝기 계산용 값이 0.0031308 이하면 12.92를 곱하고, 그보다 크면 1/2.4 거듭제곱을 포함한 식으로 sRGB 표시용 값으로 바꾼다.
    return value<=0.0031308f ? value*12.92f : 1.055f*pow(value,1.0f/2.4f)-0.055f;
}

// Canvas의 색을 곱한 sRGB 한 성분을 밝기 계산용 값으로 되돌린다. 이후 sRGB 출력 텍스처에 저장할 때 GPU가 다시 표시용 값으로 변환한다.
float ToLinear(float value)
{
    // sRGB 표시값이 0.04045 이하면 12.92로 나누고, 그보다 크면 2.4 거듭제곱을 포함한 식으로 밝기 계산용 값으로 되돌린다.
    return value<=0.04045f ? value/12.92f : pow((value+0.055f)/1.055f,2.4f);
}

// CPU가 보낸 배율·이동값으로 Canvas 픽셀 위치를 투영 후 화면 좌표에 놓고 UV·색은 그대로 전달한다.
vertex Output canvasVertex(Input input [[stage_in]],constant Transform& transform [[buffer(15)]])
{
    return {float4(input.position*transform.value.xy+transform.value.zw,0,1),input.uv,input.color};

}

// 선형 이미지의 RGB를 sRGB로 바꿔 보간된 tint의 RGBA를 곱하고, sRGB 출력일 때만 RGB를 선형으로 되돌려 화면 합성에 넘긴다.
fragment float4 canvasFragment(Output input [[stage_in]],texture2d<float> image [[texture(0)]],
    sampler imageSampler [[sampler(1)]],constant Transform& transform [[buffer(15)]])
{
    // input.uv로 image를 샘플링해 RGBA를 얻는다.
    float4 sampled=image.sample(imageSampler,input.uv);

    // 샘플 RGB를 표시용 sRGB 값으로 바꾸며 alpha는 유지한다.
    sampled.rgb=float3(ToSrgb(sampled.r),ToSrgb(sampled.g),ToSrgb(sampled.b));

    // 표시색 공간에서 정점 사이에서 섞인 곱할 색 RGBA를 샘플에 곱한다.
    float4 result=input.color*sampled;

    // sRGB 출력이라는 settings.x가 켜져 있는지 검사한다.
    if(transform.settings.x>0.5f)
        // sRGB 출력 텍스처이면 RGB를 선형화해 하드웨어 저장 변환에 전달한다.
        result.rgb=float3(ToLinear(result.r),ToLinear(result.g),ToLinear(result.b));
    return result;

}
