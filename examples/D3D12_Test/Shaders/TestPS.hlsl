cbuffer TransformData : register(b0)
{
    float4 offset;
    uint textureIndex;
    uint vertexBufferIndex;
    uint indexBufferIndex;
    uint pad1;
};

Texture2D GlobalTextures[] : register(t0, space0);
SamplerState LinearSampler : register(s0);

struct PixelInput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD;
    float3 Color : COLOR;
    float3 Normal : NORMAL;
    float3 WorldPos : TEXCOORD1;
};

float4 main(PixelInput input) : SV_TARGET
{
    // Bindless texture sampling
    float4 texColor = GlobalTextures[textureIndex].Sample(LinearSampler, input.UV);
    float4 baseColor = texColor * float4(input.Color, 1.0f);

    // 로우폴리 나무의 정교한 3D 입체감을 연출하기 위해 실시간 화면 미분으로 플랫 노멀 계산
    float3 N = normalize(cross(ddy(input.WorldPos), ddx(input.WorldPos)));

    // 디렉셔널 라이팅 연산 (오른쪽 위 대각선에서 내리쬐는 부드러운 햇빛)
    float3 L = normalize(float3(0.5f, 0.8f, -0.4f));
    float diff = max(dot(N, L), 0.0f);

    float3 ambient = float3(0.24f, 0.28f, 0.35f); // 차가운 톤의 야외 스카이 앰비언트
    float3 diffuse = float3(0.85f, 0.80f, 0.70f) * diff; // 따뜻한 햇빛 디퓨즈

    float3 finalLight = ambient + diffuse;

    return float4(baseColor.rgb * finalLight, baseColor.a);
}
