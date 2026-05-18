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

Texture2D GlobalTextures[] : register(t0, space0);
SamplerState LinearSampler : register(s0);

struct PixelInput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD0;
    float3 Color : COLOR;
    float3 WorldPos : TEXCOORD1;
    float3 Normal : NORMAL; // Vertex Shader에서 보간되어 온 정밀 법선 벡터
};

float4 main(PixelInput input) : SV_TARGET
{
    // 1. Shadow drawing (semi-transparent dark shadow)
    if (drawMode == 2)
    {
        return float4(0.01f, 0.015f, 0.025f, 0.55f); // Soft elegant dark blue-gray shadow (조금 더 은은하게 변경)
    }
    
    // 1.1. Light Markers drawing (Procedural Glowing 3D Spheres)
    if (drawMode == 3) // Spotlight marker (황금빛 구체)
    {
        float distSq = dot(input.UV, input.UV);
        if (distSq > 1.0f) discard; // 완벽한 원형으로 자름
        
        float glow = 1.0f - distSq;
        // 구체처럼 입체감을 주기 위한 페이스 셰이딩 및 림 라이트 광체 연출
        float3 glowColor = float3(1.0f, 0.85f, 0.40f) * (0.4f + 0.6f * glow * glow) + float3(1.0f, 0.95f, 0.80f) * pow(glow, 4.0f);
        return float4(glowColor, 1.0f);
    }
    if (drawMode == 4) // Directional Light marker (하늘빛/백색의 해님 구체)
    {
        float distSq = dot(input.UV, input.UV);
        if (distSq > 1.0f) discard; // 완벽한 원형으로 자름
        
        float glow = 1.0f - distSq;
        float3 glowColor = float3(0.75f, 0.90f, 1.0f) * (0.4f + 0.6f * glow * glow) + float3(1.0f, 1.0f, 1.0f) * pow(glow, 4.0f);
        return float4(glowColor, 1.0f);
    }
    
    // 2. Normal texture sample or floor solid color
    float4 baseColor = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if (drawMode == 0) // Model
    {
        baseColor = GlobalTextures[textureIndex].Sample(LinearSampler, input.UV) * float4(input.Color, 1.0f);
    }
    else if (drawMode == 1) // Floor
    {
        // Simple checkerboard pattern on the floor for dynamic effect!
        float2 grid = floor(input.WorldPos.xz * 2.0f);
        float pattern = countbits(uint(abs(grid.x) + abs(grid.y))) % 2 == 0 ? 0.82f : 1.0f; // 명암비를 살짝 더 주었습니다.
        baseColor = float4(input.Color * pattern, 1.0f);
    }
    
    // 3. Dynamic Lighting Calculations (Smooth Phong Normal Shading)
    // 보간된 버텍스 법선을 사용하여 도자기처럼 매끄럽고 고급스러운 음영 구현
    float3 N = normalize(input.Normal);
    
    // Lighting components
    float3 ambient = float3(0.20f, 0.22f, 0.28f); // Soft cool ambient light
    
    // Directional Light (swings dynamically)
    float3 L_dir = normalize(lightDir.xyz);
    float diff_dir = max(dot(N, L_dir), 0.0f);
    float3 lightColor_dir = float3(0.80f, 0.88f, 0.95f) * 0.8f; // Soft daylight
    
    // Spotlight (Warm gold cone pointing down)
    float3 spotPos = spotLightPos.xyz;
    float3 L_spot = normalize(spotPos - input.WorldPos);
    float dist = distance(spotPos, input.WorldPos);
    float3 spotDir = normalize(float3(0.0f, -1.0f, 0.0f)); // Pointing straight down
    
    float cosAngle = dot(-L_spot, spotDir);
    float intensity = 0.0f;
    if (cosAngle > 0.82f) // Spot light cone angle (approx 35 degrees)
    {
        float coneScale = (cosAngle - 0.82f) / (1.0f - 0.82f);
        intensity = coneScale * coneScale / (1.0f + 0.4f * dist * dist); // Attenuation
    }
    float3 lightColor_spot = float3(1.0f, 0.72f, 0.42f) * 2.5f * intensity;
    
    // Combine lighting
    float3 finalLight = ambient + (diff_dir * lightColor_dir) + lightColor_spot;
    
    return float4(baseColor.rgb * finalLight, baseColor.a);
}
