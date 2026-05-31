cbuffer TransformData : register(b0)
{
    float4x4 viewProj;
    float4 offset;
    float4 lightDir;
    float4 spotLightPos;
    uint textureIndex;
    uint vertexBufferIndex;
    uint indexBufferIndex;
    uint drawMode; // 0: Model, 1: Floor, 2: Shadow
    float rotationY;
    float aspect;  // 화면 뷰포트 종횡비 보정값
};

struct VertexInput
{
    float3 Pos;
    float2 UV;
    float3 Color;
    float3 Normal; // C++ Vertex::normal과 매핑
};

struct PixelInput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD0;
    float3 Color : COLOR;
    float3 WorldPos : TEXCOORD1;
    float3 Normal : NORMAL; // Pixel Shader로 넘겨줄 법선
};

StructuredBuffer<VertexInput> g_VertexBuffers[] : register(t0, space1);
StructuredBuffer<uint> g_IndexBuffers[] : register(t0, space2);

PixelInput main(uint vertexID : SV_VertexID)
{
    PixelInput output;

    // 1. Floor Drawing Mode
    if (drawMode == 1)
    {
        // 10x10 large floor at Y = 0.0f
        float3 pos[4] = {
            float3(-5.0f, 0.0f, -5.0f),
            float3( 5.0f, 0.0f, -5.0f),
            float3(-5.0f, 0.0f,  5.0f),
            float3( 5.0f, 0.0f,  5.0f)
        };
        uint indices[6] = {0, 2, 1, 1, 2, 3};
        uint vIdx = indices[vertexID % 6];

        float3 floorPos = pos[vIdx];
        output.Pos = mul(viewProj, float4(floorPos, 1.0f));
        output.UV = floorPos.xz * 0.5f;
        output.Color = float3(0.12f, 0.15f, 0.20f); // Dark elegant floor (조금 더 밝게 조절)
        output.WorldPos = floorPos;
        output.Normal = float3(0.0f, 1.0f, 0.0f); // 바닥 노멀은 항상 위쪽
        return output;
    }

    // 1.2. Billboard Sphere Drawing Mode (drawMode == 3 or 4)
    if (drawMode == 3 || drawMode == 4)
    {
        // Camera-facing billboard quad
        float2 billboardCoords[4] = {
            float2(-1.0f, -1.0f),
            float2( 1.0f, -1.0f),
            float2(-1.0f,  1.0f),
            float2( 1.0f,  1.0f)
        };
        uint quadIndices[6] = {0, 2, 1, 1, 2, 3};
        uint vIdx = quadIndices[vertexID % 6];
        float2 localCoord = billboardCoords[vIdx];

        float3 lightCenter = offset.xyz;

        // 1. 라이트 중심점을 클립 공간(Clip Space)으로 먼저 변환
        float4 clipPos = mul(viewProj, float4(lightCenter, 1.0f));

        // 2. 클립 공간에서 종횡비(C++로부터 주입받음)를 보정하여 오프셋을 더함 (찌그러짐 방지)
        // radius 배율을 3.2f로 적절히 낮춰 너무 크게 과장되지 않는 은은하고 세련된 구체를 만듭니다.
        float radius = offset.w * 3.2f;

        output.Pos = clipPos;
        output.Pos.xy += float2(localCoord.x * aspect, localCoord.y) * radius * clipPos.w;

        output.UV = localCoord; // [-1, 1] 범위로 UV 매핑
        output.Color = float3(1.0f, 1.0f, 1.0f);
        output.WorldPos = lightCenter; // 픽셀 셰이더용 임시 위치 지정
        output.Normal = float3(0.0f, 1.0f, 0.0f); // 더미
        return output;
    }

    // 2. Model & Shadow Drawing Mode
    uint realIndex = g_IndexBuffers[indexBufferIndex][vertexID];
    VertexInput input = g_VertexBuffers[vertexBufferIndex][realIndex];

    // Y축 회전 행렬 계산
    float s = sin(rotationY);
    float c = cos(rotationY);
    float3 rotatedPos = input.Pos;
    rotatedPos.x = input.Pos.x * c - input.Pos.z * s;
    rotatedPos.z = input.Pos.x * s + input.Pos.z * c;

    // 법선 벡터(Normal) 회전 변환
    float3 rotatedNormal = input.Normal;
    rotatedNormal.x = input.Normal.x * c - input.Normal.z * s;
    rotatedNormal.z = input.Normal.x * s + input.Normal.z * c;

    float3 worldPos = (rotatedPos * offset.w) + offset.xyz;

    if (drawMode == 2) // Shadow Pass
    {
        // Planar Projection Shadow mapping on floor Y = 0.0f
        float floorY = 0.0f;
        float3 L = normalize(lightDir.xyz);

        // If the light direction is pointing up, clamp to prevent divide by zero
        if (L.y < 0.001f) L.y = 0.001f;

        float t = (worldPos.y - floorY) / L.y;
        worldPos.x = worldPos.x - L.x * t;
        worldPos.y = floorY + 0.002f; // Offset slightly to avoid Z-fighting with floor
        worldPos.z = worldPos.z - L.z * t;
        output.Normal = float3(0.0f, 1.0f, 0.0f);
    }
    else
    {
        output.Normal = normalize(rotatedNormal); // 월드 공간 노멀 보관
    }

    output.Pos = mul(viewProj, float4(worldPos, 1.0f));
    output.UV = input.UV;
    output.Color = input.Color;
    output.WorldPos = worldPos;

    return output;
}
