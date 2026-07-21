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
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
    float4 tangent [[attribute(3)]];
};

inline RasterData RunVertexShader(
    MeshVertex vertex,
    constant DrawConstants& drawConstants,
    constant ShadowMatrix& shadowMatrix,
    device const float4x4* instanceTransforms,
    uint instanceId)
{
    float4x4 resolvedModelMatrix = drawConstants.modelMatrix;
    if (drawConstants.instanceTransformOffset != 0u)
    {
        resolvedModelMatrix =
            instanceTransforms[drawConstants.instanceTransformOffset - 1u + instanceId];
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
    MeshVertex vertex [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]],
    device const float4x4* instanceTransforms [[buffer(11)]],
    uint instanceId [[instance_id]])
{
    return RunVertexShader(
        vertex,
        drawConstants,
        shadowMatrix,
        instanceTransforms,
        instanceId);
}

// Stock Renderer가 ShaderDesc에서 명시적으로 선택하는 entry point.
vertex RasterData main0(
    MeshVertex vertex [[stage_in]],
    constant DrawConstants& drawConstants [[buffer(0)]],
    constant ShadowMatrix& shadowMatrix [[buffer(3)]],
    device const float4x4* instanceTransforms [[buffer(11)]],
    uint instanceId [[instance_id]])
{
    return RunVertexShader(
        vertex,
        drawConstants,
        shadowMatrix,
        instanceTransforms,
        instanceId);
}
