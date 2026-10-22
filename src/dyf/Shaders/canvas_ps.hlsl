Texture2D image:register(t0);
SamplerState imageSampler:register(s1);

// 정점 변환용 배율·이동과 픽셀 출력의 sRGB 여부를 함께 담는 Canvas 상수다. 두 단계가 같은 배치를 공유한다.
cbuffer Transform:register(b15) { float4 transform; float4 settings; };

// 선형 RGB의 한 성분을 Canvas 지정 색과 같은 sRGB 값으로 바꾼다. 호출부가 RGB에만 적용하므로 알파는 유지된다.
float ToSrgb(float value)
{
    // 밝기 계산용 값이 0.0031308 이하면 일정 배율을 곱하고, 그보다 크면 거듭제곱 식으로 sRGB 표시용 값으로 바꾼다.
    return value<=0.0031308 ? value*12.92 : 1.055*pow(value,1.0/2.4)-0.055;
}

// Canvas의 색을 곱한 sRGB 한 성분을 밝기 계산용 값으로 되돌린다. 이후 sRGB 출력 텍스처에 저장할 때 GPU가 다시 표시용 값으로 변환한다.
float ToLinear(float value)
{
    // sRGB 표시값이 0.04045 이하면 나눗셈을, 그보다 크면 2.4 거듭제곱을 포함한 식을 써서 밝기 계산용 값으로 되돌린다.
    return value<=0.04045 ? value/12.92 : pow((value+0.055)/1.055,2.4);
}

// 선형 이미지의 RGB를 sRGB로 바꿔 보간된 tint의 RGBA를 곱하고, sRGB 출력일 때만 RGB를 선형으로 되돌려 화면 합성에 넘긴다.
float4 main(float4 position:SV_POSITION,float2 uv:TEXCOORD0,float4 color:TEXCOORD1):SV_TARGET
{
    // imageSampler로 UV의 선형 이미지 RGBA를 읽는다.
    float4 sampled=image.Sample(imageSampler,uv);

    // RGB를 ToSrgb로 표시색 공간에 옮기며 alpha는 그대로 둔다.
    sampled.rgb=float3(ToSrgb(sampled.r),ToSrgb(sampled.g),ToSrgb(sampled.b));

    // 샘플과 곱할 색의 RGBA를 곱한 결과를 만든다.
    float4 result=color*sampled;

    // settings.x가 sRGB 출력 사용을 나타내는지 검사한다.
    if(settings.x>0.5)
        // sRGB 출력이면 RGB만 ToLinear로 되돌려 하드웨어 저장 변환 전에 선형 값을 만든다.
        result.rgb=float3(ToLinear(result.r),ToLinear(result.g),ToLinear(result.b));
    return result;

}
