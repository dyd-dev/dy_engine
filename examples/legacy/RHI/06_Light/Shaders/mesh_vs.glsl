#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 1) out vec3 fragWorldPosition;
layout(location = 2) out vec3 fragNormal;

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

// 특이 변환에서는 기존처럼 단위행렬을 법선 변환에 사용한다.
mat3 MeshNormalMatrix(mat4 matrix)
{
    vec3 a = matrix[0].xyz;
    vec3 b = matrix[1].xyz;
    vec3 c = matrix[2].xyz;
    float det = dot(a, cross(b, c));
    if (abs(det) <= 0.00000001) return mat3(1.0);
    return mat3(cross(b, c) / det, cross(c, a) / det, cross(a, b) / det);
}

void main()
{
    mat4 world = drawConstants.modelMatrix;
    mat3 normalMatrix = MeshNormalMatrix(world);
    vec4 worldPosition = world * vec4(inPosition, 1.0);
    gl_Position = drawConstants.viewProjectionMatrix * worldPosition;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(normalMatrix) * inNormal);
}
