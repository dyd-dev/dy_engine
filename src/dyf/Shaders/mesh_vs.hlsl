// 이 셰이더가 사용하는 바인딩과 입력 크기다.
#define RENDERER_MAX_DIRECTIONAL_LIGHTS 4
#define RENDERER_MAX_POINT_LIGHTS 16
#define RENDERER_MAX_SPOT_LIGHTS 16

#ifndef RENDERER_ENABLE_SHADOWS
#error RENDERER_ENABLE_SHADOWS must be defined
#endif

cbuffer DrawConstants : register(
    b10,
    space0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};

#if RENDERER_ENABLE_SHADOWS
cbuffer ShadowMatrix : register(
    b3,
    space0)
{
    float4x4 lightViewProjectionMatrix[128];
    float4 atlasRect[128];
    float4 directionalViews[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    float4 pointViews[RENDERER_MAX_POINT_LIGHTS];
    float4 spotViews[RENDERER_MAX_SPOT_LIGHTS];
    float4 directionalSplits[RENDERER_MAX_DIRECTIONAL_LIGHTS];
    float4x4 cameraView;
    float4 filterParams;
};
#endif

struct VSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;
#if RENDERER_INSTANCING
    float4 model0 : TEXCOORD4;
    float4 model1 : TEXCOORD5;
    float4 model2 : TEXCOORD6;
    float4 model3 : TEXCOORD7;
#endif
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float4 worldTangent : TEXCOORD3;
#if RENDERER_ENABLE_SHADOWS
    float4 lightSpacePosition : TEXCOORD4;
#endif
};

// 특이 변환에서는 기존처럼 단위행렬을 법선 변환에 사용한다.
float3 MeshNormalizeOr(float3 value, float3 fallback)
{
    float scale = max(max(abs(value.x),abs(value.y)),abs(value.z));
    if (!(scale > 0.0) || scale > 3.402823466e38) return fallback;
    value /= scale;
    return value / length(value);
}

float3 MeshFallbackTangent(float3 normal)
{
    float3 up = abs(normal.z) < 0.999 ? float3(0,0,1) : float3(0,1,0);
    return MeshNormalizeOr(cross(up,normal),float3(1,0,0));
}

float3x3 MeshNormalMatrix(float4x4 matrix)
{
    float3 a = float3(matrix[0][0], matrix[1][0], matrix[2][0]);
    float3 b = float3(matrix[0][1], matrix[1][1], matrix[2][1]);
    float3 c = float3(matrix[0][2], matrix[1][2], matrix[2][2]);
    float scale = max(max(max(abs(a.x),abs(a.y)),abs(a.z)),
        max(max(max(abs(b.x),abs(b.y)),abs(b.z)),max(max(abs(c.x),abs(c.y)),abs(c.z))));
    if (scale == 0.0) return float3x3(1,0,0, 0,1,0, 0,0,1);
    a /= scale; b /= scale; c /= scale;
    float det = dot(a, cross(b, c));
    if (det == 0.0) return float3x3(1,0,0, 0,1,0, 0,0,1);
    // Normals are normalized below; omit the common positive inverse scale.
    float orientation = det < 0.0 ? -1.0 : 1.0;
    return transpose(float3x3(cross(b, c) * orientation, cross(c, a) * orientation, cross(a, b) * orientation));
}

VSOutput main(VSInput input)
{
#if RENDERER_INSTANCING
    const float4x4 world = transpose(float4x4(input.model0,input.model1,input.model2,input.model3));
#else
    const float4x4 world = modelMatrix;
#endif
    const float4 worldPosition = mul(world, float4(input.position, 1.0));
    const float3x3 normalMatrix = MeshNormalMatrix(world);

    VSOutput output;
    output.position = mul(viewProjectionMatrix, worldPosition);
    output.uv = input.uv;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = MeshNormalizeOr(mul(normalMatrix, input.normal), float3(0,0,1));
    float3 tangent = mul((float3x3)world, input.tangent.xyz);
    tangent = MeshNormalizeOr(tangent - output.worldNormal * dot(output.worldNormal, tangent), MeshFallbackTangent(output.worldNormal));
    output.worldTangent = float4(tangent, determinant((float3x3)world) < 0.0 ? -input.tangent.w : input.tangent.w);
#if RENDERER_ENABLE_SHADOWS
    output.lightSpacePosition = mul(lightViewProjectionMatrix[0], worldPosition);
#endif
    return output;
}
