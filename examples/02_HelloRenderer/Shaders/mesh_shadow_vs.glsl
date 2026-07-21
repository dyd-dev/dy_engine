#version 450
#extension GL_GOOGLE_include_directive : require

#include "Graphics/RendererShaderLayout.inc"

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform DrawConstants {
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    float drawMode;
    uint instanceTransformOffset;
    vec3 emissiveColor;
    float baseColorTextureIndex;
    vec4 baseColor;
    vec4 materialParams;
} pushConstants;

layout(set = 0, binding = DY_RENDERER_BINDING_SHADOW_MATRIX) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrix;
} shadowMatrix;

void main() {
    if ((int(pushConstants.drawMode + 0.5) & DY_RENDERER_TEXTURE_FLAG_CAST_SHADOW) == 0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    vec4 worldPosition = pushConstants.modelMatrix * vec4(inPosition, 1.0);
    gl_Position = shadowMatrix.lightViewProjectionMatrix * worldPosition;
}
