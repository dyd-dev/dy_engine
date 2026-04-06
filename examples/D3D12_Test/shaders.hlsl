// Texture2D t1 : register(t0);
// SamplerState s1 : register(s0);
cbuffer TransformDate : register(b0)
{
    float4 offset;
};

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
    
    float3 finalPos = (input.Pos * 0.1f) + offset.xyz;
    
    output.Pos = float4(finalPos, 1.0f);
    output.UV = input.UV;
    
    return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
    // 일단 텍스처 없이 흰색(1, 1, 1, 1)으로 칠해서 모델 형태를 확인합니다.
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}