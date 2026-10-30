#version 450
#extension GL_GOOGLE_include_directive : require

#include "ModelShaderLayout.inc"
#include "Skinning.glsl"

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform DrawConstants
{
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    vec4 emissiveColor;
    vec4 baseColor;
    vec4 materialParams;
    // Model 확장이 추가한 상수이며 기본 Renderer의 padding을 사용하지 않는다.
    uint influenceOffset;
    uint paletteOffset;
    // Metal 상수 구조체의 16바이트 정렬을 포함하여 추가 영역을 16바이트로 맞춘다.
    uint modelPadding0;
    uint modelPadding1;
} drawConstants;

layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix
{
    #define DY_SHADOW_MATRIX mat4
#define DY_SHADOW_FLOAT4 vec4
#include "ModelShadowFields.inc"
#undef DY_SHADOW_MATRIX
#undef DY_SHADOW_FLOAT4
} shadowMatrix;

void main()
{
    mat4 skin, skinNormal;
    LoadSkinning(gl_VertexIndex, drawConstants.influenceOffset, drawConstants.paletteOffset, skin, skinNormal);
    gl_Position = shadowMatrix.lightViewProjectionMatrix[drawConstants.padding2] * drawConstants.modelMatrix * skin * vec4(inPosition, 1.0);
}
