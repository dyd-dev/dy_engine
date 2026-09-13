#version 450
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 worldNormal;

layout(push_constant) uniform DrawConstants
{
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    vec4 baseColor;
    float metallic;
    float roughness;
} draw;

// 비균일 배율을 포함한 법선 변환이며 특이 행렬에서는 단위행렬을 사용한다.
mat3 MeshNormalMatrix(mat4 matrix)
{
    vec3 a = matrix[0].xyz, b = matrix[1].xyz, c = matrix[2].xyz;
    float determinant = dot(a, cross(b, c));
    if(abs(determinant) <= 0.00000001) return mat3(1.0);
    return mat3(cross(b, c) / determinant, cross(c, a) / determinant, cross(a, b) / determinant);
}

void main()
{
    vec4 world = draw.modelMatrix * vec4(inPosition, 1.0);
    gl_Position = draw.viewProjectionMatrix * world;
    worldPosition = world.xyz;
    worldNormal = normalize(MeshNormalMatrix(draw.modelMatrix) * inNormal);
}
