cbuffer TransformData : register(b0)
{
    float4 offset;
    uint textureIndex;
    uint vertexBufferIndex;
    uint indexBufferIndex;
    uint pad1;
};

struct VertexInput
{
    float3 Pos;
    float2 UV;
    float3 Color;
    float3 Normal; // 44바이트 스트라이드 매핑용 추가
};

struct PixelInput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD;
    float3 Color : COLOR;
    float3 Normal : NORMAL;
    float3 WorldPos : TEXCOORD1;
};

StructuredBuffer<VertexInput> g_VertexBuffers[] : register(t0, space1);
StructuredBuffer<uint> g_IndexBuffers[] : register(t0, space2);

PixelInput main(uint vertexID : SV_VertexID)
{
    PixelInput output;
    
    uint realIndex = g_IndexBuffers[indexBufferIndex][vertexID];
    VertexInput input = g_VertexBuffers[vertexBufferIndex][realIndex];
    
    // 1. 모델 좌표에 스케일 및 오프셋 적용하여 3D 카메라 뷰 공간 좌표 생성
    // (스케일을 0.10f로 크게 키우고, 오른쪽으로 쏠린 가지들의 균형을 위해 X를 왼쪽(-0.15f)으로 미세 이동하여 꽉 찬 중앙 배치 구현)
    float3 viewPos = (input.Pos * 0.10f) + float3(offset.x - 0.15f, offset.y - 0.3f, offset.z + 2.0f);
    
    // 2. 원근 투영 (Perspective Projection) 직접 연산
    float fovScale = 1.1f; // 시야각(FoV) 스케일
    float aspect = 800.0f / 600.0f; // 화면 종횡비
    
    output.Pos.x = viewPos.x / (aspect * fovScale);
    output.Pos.y = viewPos.y / fovScale;
    
    // 깊이 버퍼 동작을 위한 원근 투영 Z 변환 [0, 1] 범위 매핑
    float nearZ = 0.1f;
    float farZ = 20.0f;
    output.Pos.z = (viewPos.z * (farZ / (farZ - nearZ)) - (farZ * nearZ / (farZ - nearZ)));
    output.Pos.w = viewPos.z; // W 채널에 깊이를 대입하여 GPU가 자동으로 원근 분할 수행
    
    output.UV = input.UV;
    output.Color = input.Color;
    output.Normal = input.Normal;
    output.WorldPos = viewPos;
    
    return output;
}
