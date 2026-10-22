#version 450
// Canvas가 현재 그리는 이미지와 그 샘플링 방법이다. 픽셀에 맞게 계산된 UV 위치의 RGBA를 읽는다.
layout(set=0,binding=0) uniform texture2D image;
layout(set=0,binding=1) uniform sampler imageSampler;

// value의 배율·이동은 정점 위치 변환에, settings.x의 sRGB 여부는 픽셀 색 출력에 쓰이는 공통 상수다.
layout(push_constant) uniform Transform { vec4 value; vec4 settings; } transform;

// 정점 단계가 넘긴 UV와 곱할 색이 각 픽셀 위치에 맞게 섞여 들어온다. 아래 result에 이미지와 색을 곱한 RGBA를 쓴다.
layout(location=0) in vec2 uv;
layout(location=1) in vec4 color;
layout(location=0) out vec4 result;

// 선형 RGB의 한 성분을 Canvas 지정 색과 같은 sRGB 값으로 바꾼다. 호출부가 RGB에만 적용하므로 알파는 유지된다.
float ToSrgb(float value)
{
    return value<=0.0031308 ? value*12.92 : 1.055*pow(value,1.0/2.4)-0.055;
}

// Canvas의 색을 곱한 sRGB 한 성분을 밝기 계산용 값으로 되돌린다. 이후 sRGB 출력 텍스처에 저장할 때 GPU가 다시 표시용 값으로 변환한다.
float ToLinear(float value)
{
    return value<=0.04045 ? value/12.92 : pow((value+0.055)/1.055,2.4);
}

// 선형 이미지의 RGB를 sRGB로 바꿔 보간된 tint의 RGBA를 곱하고, sRGB 출력일 때만 RGB를 선형으로 되돌려 화면 합성에 넘긴다.
void main()
{
    // 분리된 image와 sampler를 sampler2D로 결합해 UV 위치의 RGBA를 읽는다.
    vec4 sampled=texture(sampler2D(image,imageSampler),uv);

    sampled.rgb=vec3(ToSrgb(sampled.r),ToSrgb(sampled.g),ToSrgb(sampled.b));

    result=color*sampled;

    if(transform.settings.x>0.5)
        result.rgb=vec3(ToLinear(result.r),ToLinear(result.g),ToLinear(result.b));

}
