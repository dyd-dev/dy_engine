#version 450

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform DrawConstants
{
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    vec4 baseColor;
    float metallic;
    float roughness;
    uint receiveShadow;
    uint shadowViewIndex;
} drawConstants;

layout(set = 0, binding = 3) uniform ShadowMatrix
{
// 정적 예제의 방향광 1개·점광원 6면·스폿 광원 1개를 같은 깊이 아틀라스에 저장한다.
mat4 lightViewProjectionMatrix[8];
vec4 atlasRect[8];
vec4 directionalViews[1];
vec4 pointViews[1];
vec4 spotViews[1];

} shadowMatrix;

void main()
{
    gl_Position = shadowMatrix.lightViewProjectionMatrix[drawConstants.shadowViewIndex] * drawConstants.modelMatrix * vec4(inPosition, 1.0);
}
