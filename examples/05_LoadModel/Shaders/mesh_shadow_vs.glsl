#version 450
#extension GL_GOOGLE_include_directive : require

#include "Graphics/RendererShaderLayout.inc"
#ifndef DY_SKINNING_INFLUENCE_BINDING
#define DY_SKINNING_INFLUENCE_BINDING DY_RENDERER_BINDING_SKIN_INFLUENCE_STORAGE
#endif

#ifndef DY_SKINNING_PALETTE_BINDING
#define DY_SKINNING_PALETTE_BINDING DY_RENDERER_BINDING_SKIN_PALETTE_STORAGE
#endif

struct SkinInfluence {
    uvec4 joints;
    vec4 weights;
    float dqBlendWeight;
    float _padding0;
    float _padding1;
    float _padding2;
};

layout(std430, set = 0, binding = DY_SKINNING_INFLUENCE_BINDING) readonly buffer SkinInfluenceStorage {
    SkinInfluence influences[];
} skinInfluenceStorage;

struct SkinJointMatrices {
    mat4 positionMatrix;
    mat4 normalMatrix;
    vec4 dqReal;
    vec4 dqDual;
    vec4 dqScale;
};

layout(std430, set = 0, binding = DY_SKINNING_PALETTE_BINDING) readonly buffer SkinPaletteStorage {
    SkinJointMatrices joints[];
} skinPaletteStorage;

vec4 QuaternionMultiply(vec4 lhs, vec4 rhs) {
    return vec4(
        lhs.w * rhs.xyz + rhs.w * lhs.xyz + cross(lhs.xyz, rhs.xyz),
        lhs.w * rhs.w - dot(lhs.xyz, rhs.xyz));
}

mat3 QuaternionRotation(vec4 quaternion) {
    vec4 q = normalize(quaternion);
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return mat3(
        vec3(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz), 2.0 * (xz - wy)),
        vec3(2.0 * (xy - wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx)),
        vec3(2.0 * (xz + wy), 2.0 * (yz - wx), 1.0 - 2.0 * (xx + yy)));
}

mat4 LoadLinearSkinMatrix(SkinInfluence influence, uint paletteOffset, vec4 weights) {
    return skinPaletteStorage.joints[paletteOffset + influence.joints.x].positionMatrix * weights.x
         + skinPaletteStorage.joints[paletteOffset + influence.joints.y].positionMatrix * weights.y
         + skinPaletteStorage.joints[paletteOffset + influence.joints.z].positionMatrix * weights.z
         + skinPaletteStorage.joints[paletteOffset + influence.joints.w].positionMatrix * weights.w;
}

mat4 LoadDualQuaternionSkinMatrix(SkinInfluence influence, uint paletteOffset, vec4 weights) {
    uint firstComponent = weights.x > 0.0 ? 0u : (weights.y > 0.0 ? 1u : (weights.z > 0.0 ? 2u : 3u));
    uint firstJoint = influence.joints[firstComponent];
    vec4 referenceReal = skinPaletteStorage.joints[paletteOffset + firstJoint].dqReal;
    vec4 realSum = vec4(0.0);
    vec4 dualSum = vec4(0.0);
    vec3 scaleSum = vec3(0.0);
    for(uint component = 0u; component < 4u; ++component) {
        float weight = weights[component];
        if(weight <= 0.0) continue;
        SkinJointMatrices joint = skinPaletteStorage.joints[paletteOffset + influence.joints[component]];
        float signCorrection = dot(referenceReal, joint.dqReal) < 0.0 ? -1.0 : 1.0;
        realSum += joint.dqReal * (weight * signCorrection);
        dualSum += joint.dqDual * (weight * signCorrection);
        scaleSum += joint.dqScale.xyz * weight;
    }
    float realLength = length(realSum);
    if(realLength <= 1.0e-6) return mat4(1.0);
    vec4 real = realSum / realLength;
    vec4 dual = dualSum / realLength;
    vec4 conjugateReal = vec4(-real.xyz, real.w);
    vec3 translation = 2.0 * QuaternionMultiply(dual, conjugateReal).xyz;
    mat3 rotation = QuaternionRotation(real);
    mat4 result = mat4(1.0);
    result[0].xyz = rotation[0] * scaleSum.x;
    result[1].xyz = rotation[1] * scaleSum.y;
    result[2].xyz = rotation[2] * scaleSum.z;
    result[3].xyz = translation;
    return result;
}

mat4 LoadSkinMatrix(uint vertexIndex, uint paletteOffset) {
    SkinInfluence influence = skinInfluenceStorage.influences[vertexIndex];
    float total = dot(influence.weights, vec4(1.0));
    if (total <= 1.0e-6) return mat4(1.0);

    vec4 weights = influence.weights / total;
    mat4 linearMatrix = LoadLinearSkinMatrix(influence, paletteOffset, weights);
    if(influence.dqBlendWeight <= 0.0) return linearMatrix;
    mat4 dualQuaternionMatrix = LoadDualQuaternionSkinMatrix(influence, paletteOffset, weights);
    float dqBlendWeight = clamp(influence.dqBlendWeight, 0.0, 1.0);
    return linearMatrix + (dualQuaternionMatrix - linearMatrix) * dqBlendWeight;
}

mat3 LoadSkinNormalMatrix(uint vertexIndex, uint paletteOffset) {
    SkinInfluence influence = skinInfluenceStorage.influences[vertexIndex];
    float total = dot(influence.weights, vec4(1.0));
    if (total <= 1.0e-6) return mat3(1.0);

    vec4 weights = influence.weights / total;
    mat3 finalLinear = mat3(LoadSkinMatrix(vertexIndex, paletteOffset));
    float linearDeterminant = determinant(finalLinear);
    if(abs(linearDeterminant) > 1.0e-8) return transpose(inverse(finalLinear));
    return mat3(skinPaletteStorage.joints[paletteOffset + influence.joints.x].normalMatrix) * weights.x
         + mat3(skinPaletteStorage.joints[paletteOffset + influence.joints.y].normalMatrix) * weights.y
         + mat3(skinPaletteStorage.joints[paletteOffset + influence.joints.z].normalMatrix) * weights.z
         + mat3(skinPaletteStorage.joints[paletteOffset + influence.joints.w].normalMatrix) * weights.w;
}

layout(std140, set = 0, binding = DY_VULKAN_BINDING_DRAW_CONSTANTS) uniform VulkanDrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    float drawMode;
    uint firstIndex;
    int vertexOffset;
    uint firstVertex;
    vec3 emissiveColor;
    float emissiveTextureIndex;
    vec4 baseColor;
    vec4 materialParams;
    vec4 textureIndices;
} pushConstants;

layout(set = 0, binding = DY_RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrix;
} shadowMatrix;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_VERTEX_STORAGE) readonly buffer VertexStorage {
    float vertices[];
} vertexStorage;

layout(std430, set = 0, binding = DY_RENDERER_BINDING_INDEX_STORAGE) readonly buffer IndexStorage {
    uint indices[];
} indexStorage;

vec3 LoadPosition(uint vertexIndex) {
    uint base = vertexIndex * DY_RENDERER_VERTEX_FLOAT_COUNT;
    return vec3(
        vertexStorage.vertices[base + 0u],
        vertexStorage.vertices[base + 1u],
        vertexStorage.vertices[base + 2u]);
}

void main() {
    if ((int(pushConstants.drawMode + 0.5) & DY_RENDERER_TEXTURE_FLAG_CAST_SHADOW) == 0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    int resolvedVertexIndex = int(indexStorage.indices[pushConstants.firstIndex + uint(gl_VertexIndex)]) + pushConstants.vertexOffset;
    vec3 localPosition = LoadPosition(uint(resolvedVertexIndex));
    if ((uint(pushConstants.drawMode + 0.5) & DY_RENDERER_TEXTURE_FLAG_SKINNED) != 0u) {
        localPosition = (LoadSkinMatrix(uint(resolvedVertexIndex), pushConstants.firstVertex) * vec4(localPosition, 1.0)).xyz;
    }
    vec4 worldPosition = pushConstants.modelMatrix * vec4(localPosition, 1.0);
    gl_Position = shadowMatrix.lightViewProjectionMatrix * worldPosition;
}
