#version 450
#extension GL_GOOGLE_include_directive : require

#include "Graphics/ShaderInterop/StockShaderLayout.inc"
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
} drawConstants;

layout(set = RENDERER_DESCRIPTOR_SET, binding = RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix
{
    mat4 lightViewProjectionMatrix;
} shadowMatrix;

void main()
{
    mat4 skin, skinNormal;
    LoadSkinning(gl_VertexIndex, drawConstants.padding0, drawConstants.padding1, skin, skinNormal);
    gl_Position = shadowMatrix.lightViewProjectionMatrix * drawConstants.modelMatrix * skin * vec4(inPosition, 1.0);
}
