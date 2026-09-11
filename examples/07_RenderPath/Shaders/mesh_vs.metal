#include <metal_stdlib>
using namespace metal;

// Vulkan's RenderPath shader can replace the per-draw model matrix with a
// matrix from the instance buffer. Metal uses the same binding (11).
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

struct VertexOutput
{
    float4 position [[position]];
    float2 uv [[user(locn0)]];
    float3 worldPosition [[user(locn1)]];
    float3 worldNormal [[user(locn2)]];
    float4 worldTangent [[user(locn3)]];
};

vertex VertexOutput main0(
    uint vertexId [[vertex_id]],
    uint instanceId [[instance_id]],
    constant DrawConstants& draw [[buffer(0)]],
    device const float* vertices [[buffer(4)]],
    device const uint* indices [[buffer(5)]],
    device const float4x4* instanceTransforms [[buffer(11)]])
{
    constexpr uint floatsPerVertex = 12u;
    const int resolvedIndex = int(indices[draw.firstIndex + vertexId]) + draw.vertexOffset;
    const uint base = uint(resolvedIndex) * floatsPerVertex;
    const float3 localPosition = float3(vertices[base], vertices[base + 1u], vertices[base + 2u]);
    const float3 localNormal = float3(vertices[base + 3u], vertices[base + 4u], vertices[base + 5u]);
    const float2 uv = float2(vertices[base + 6u], vertices[base + 7u]);
    const float4 localTangent = float4(
        vertices[base + 8u], vertices[base + 9u], vertices[base + 10u], vertices[base + 11u]);

    float4x4 modelMatrix = draw.modelMatrix;
    if(draw.firstVertex != 0u)
        modelMatrix = instanceTransforms[draw.firstVertex - 1u + instanceId];

    const float4 worldPosition = modelMatrix * float4(localPosition, 1.0f);
    const float3x3 normalMatrix = float3x3(modelMatrix[0].xyz, modelMatrix[1].xyz, modelMatrix[2].xyz);

    VertexOutput output;
    output.position = draw.viewProjectionMatrix * worldPosition;
    output.uv = uv;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(normalMatrix * localNormal);
    output.worldTangent = float4(normalize(normalMatrix * localTangent.xyz), localTangent.w);
    return output;
}
