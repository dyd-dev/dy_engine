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

struct RasterData
{
    float4 position [[position]];
    float2 uv [[user(locn0)]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
    float4 worldTangent [[user(locn3)]];
    float4 lightSpacePosition [[user(locn4)]];
};

struct MeshVertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float4 tangent;
};

constant uint kRendererVertexFloatCount = 12u;

inline MeshVertex LoadVertex(device const float* vertices, uint vertexIndex)
{
    const uint base = vertexIndex * kRendererVertexFloatCount;
    MeshVertex vertex;
    vertex.position = float3(vertices[base + 0u], vertices[base + 1u], vertices[base + 2u]);
    vertex.normal = float3(vertices[base + 3u], vertices[base + 4u], vertices[base + 5u]);
    vertex.uv = float2(vertices[base + 6u], vertices[base + 7u]);
    vertex.tangent = float4(
        vertices[base + 8u],
        vertices[base + 9u],
        vertices[base + 10u],
        vertices[base + 11u]);
    return vertex;
}

inline RasterData RunVertexShader(
    constant DrawConstants& drawConstants,
    constant ShadowMatrix& shadowMatrix,
    device const float* vertices,
    device const uint* indices,
    device const float4x4* instanceTransforms,
    uint vertexId,
    uint instanceId)
{
    const int resolvedVertexIndex =
        int(indices[drawConstants.firstIndex + vertexId]) + drawConstants.vertexOffset;
    const MeshVertex vertex = LoadVertex(vertices, uint(resolvedVertexIndex));

    float4x4 resolvedModelMatrix = drawConstants.modelMatrix;
    if (drawConstants.firstVertex != 0u)
    {
        resolvedModelMatrix =
            instanceTransforms[drawConstants.firstVertex - 1u + instanceId];
    }

    const float4 worldPosition = resolvedModelMatrix * float4(vertex.position, 1.0f);
    const float3x3 normalMatrix = float3x3(
        resolvedModelMatrix[0].xyz,
        resolvedModelMatrix[1].xyz,
        resolvedModelMatrix[2].xyz);

    RasterData output;
    output.position = drawConstants.viewProjectionMatrix * worldPosition;
    output.uv = vertex.uv;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(normalMatrix * vertex.normal);
    output.worldTangent =
        float4(normalize(normalMatrix * vertex.tangent.xyz), vertex.tangent.w);
    output.lightSpacePosition =
        shadowMatrix.lightViewProjectionMatrix * worldPosition;
    return output;
}

vertex RasterData vertexShader(
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]],
    device const float* vertices [[buffer(4)]],
    device const uint* indices [[buffer(5)]],
    device const float4x4* instanceTransforms [[buffer(11)]],
    uint vertexId [[vertex_id]],
    uint instanceId [[instance_id]])
{
    return RunVertexShader(
        drawConstants,
        shadowMatrix,
        vertices,
        indices,
        instanceTransforms,
        vertexId,
        instanceId);
}

// Stock Renderer가 ShaderDesc에서 명시적으로 선택하는 entry point.
vertex RasterData main0(
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]],
    device const float* vertices [[buffer(4)]],
    device const uint* indices [[buffer(5)]],
    device const float4x4* instanceTransforms [[buffer(11)]],
    uint vertexId [[vertex_id]],
    uint instanceId [[instance_id]])
{
    return RunVertexShader(
        drawConstants,
        shadowMatrix,
        vertices,
        indices,
        instanceTransforms,
        vertexId,
        instanceId);
}
