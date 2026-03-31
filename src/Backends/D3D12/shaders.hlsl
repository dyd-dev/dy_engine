// 1. 텍스처와 샘플러(이미지를 어떻게 읽을지 결정하는 도구) 선언
Texture2D t1 : register(t0);
SamplerState s1 : register(s0);

struct VertexInput
{
    float3 Pos : POSITION;
    float2 UV : TEXCOORD;
};

struct PixelInput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD;
};

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    output.Pos = float4(input.Pos, 1.0f);
    output.UV = input.UV;
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    // 이제 색상을 계산하지 않고, 텍스처에서 해당 UV 좌표의 픽셀 색을 뽑아옵니다
    return t1.Sample(s1, input.UV);
}