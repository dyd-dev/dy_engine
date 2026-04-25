struct PixelInput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD;
};

float4 main(PixelInput input) : SV_TARGET
{
    // 일단 텍스처 없이 흰색(1, 1, 1, 1)으로 칠해서 모델 형태를 확인합니다.
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
