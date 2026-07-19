#include <metal_stdlib>

using namespace metal;

struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
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
constant uint kRendererVertexFloatCount = 12u;

inline float4 RunShadowVertexShader(
    constant DrawConstants& drawConstants,
    constant ShadowMatrix& shadowMatrix,
    device const float* vertices,
    device const uint* indices,
    device const float4x4* instanceTransforms,
    uint vertexId,
    uint instanceId)
{
    if ((uint(drawConstants.drawMode + 0.5f) &
        kTextureFlagCastShadow) == 0u)
    {
        return float4(2.0f, 2.0f, 2.0f, 1.0f);
    }

    const int resolvedVertexIndex =
        int(indices[drawConstants.firstIndex + vertexId]) +
        drawConstants.vertexOffset;
    const uint base =
        uint(resolvedVertexIndex) * kRendererVertexFloatCount;
    const float3 position = float3(
        vertices[base + 0u],
        vertices[base + 1u],
        vertices[base + 2u]);

    float4x4 resolvedModelMatrix = drawConstants.modelMatrix;
    if (drawConstants.firstVertex != 0u)
    {
        resolvedModelMatrix =
            instanceTransforms[drawConstants.firstVertex - 1u + instanceId];
    }

    const float4 worldPosition =
        resolvedModelMatrix * float4(position, 1.0f);
    return shadowMatrix.lightViewProjectionMatrix * worldPosition;
}

vertex float4 vertexShader(
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]],
    device const float* vertices [[buffer(4)]],
    device const uint* indices [[buffer(5)]],
    device const float4x4* instanceTransforms [[buffer(11)]],
    uint vertexId [[vertex_id]],
    uint instanceId [[instance_id]]) [[position]]
{
    return RunShadowVertexShader(
        drawConstants,
        shadowMatrix,
        vertices,
        indices,
        instanceTransforms,
        vertexId,
        instanceId);
}

vertex float4 shadowVertexShader(
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]],
    device const float* vertices [[buffer(4)]],
    device const uint* indices [[buffer(5)]],
    device const float4x4* instanceTransforms [[buffer(11)]],
    uint vertexId [[vertex_id]],
    uint instanceId [[instance_id]]) [[position]]
{
    return RunShadowVertexShader(
        drawConstants,
        shadowMatrix,
        vertices,
        indices,
        instanceTransforms,
        vertexId,
        instanceId);
}

// Stock Renderer가 ShaderDesc에서 명시적으로 선택하는 entry point.
vertex float4 main0(
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]],
    device const float* vertices [[buffer(4)]],
    device const uint* indices [[buffer(5)]],
    device const float4x4* instanceTransforms [[buffer(11)]],
    uint vertexId [[vertex_id]],
    uint instanceId [[instance_id]]) [[position]]
{
    return RunShadowVertexShader(
        drawConstants,
        shadowMatrix,
        vertices,
        indices,
        instanceTransforms,
        vertexId,
        instanceId);
}
