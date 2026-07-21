#include <metal_stdlib>

using namespace metal;

struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    float drawMode;
    uint instanceTransformOffset;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
    float4 textureIndices;
};

struct ShadowMatrix
{
    float4x4 lightViewProjectionMatrix;
};

constant uint kTextureFlagCastShadow = 64u;

struct ShadowVertex
{
    float3 position [[attribute(0)]];
};

inline float4 RunShadowVertexShader(
    ShadowVertex vertex,
    constant DrawConstants& drawConstants,
    constant ShadowMatrix& shadowMatrix)
{
    if ((uint(drawConstants.drawMode + 0.5f) &
        kTextureFlagCastShadow) == 0u)
    {
        return float4(2.0f, 2.0f, 2.0f, 1.0f);
    }
    const float4 worldPosition =
        drawConstants.modelMatrix * float4(vertex.position, 1.0f);
    return shadowMatrix.lightViewProjectionMatrix * worldPosition;
}

vertex float4 vertexShader(
    ShadowVertex vertex [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]]) [[position]]
{
    return RunShadowVertexShader(
        vertex,
        drawConstants,
        shadowMatrix);
}

vertex float4 shadowVertexShader(
    ShadowVertex vertex [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]]) [[position]]
{
    return RunShadowVertexShader(
        vertex,
        drawConstants,
        shadowMatrix);
}

// Stock Renderer가 ShaderDesc에서 명시적으로 선택하는 entry point.
vertex float4 main0(
    ShadowVertex vertex [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]]) [[position]]
{
    return RunShadowVertexShader(
        vertex,
        drawConstants,
        shadowMatrix);
}
