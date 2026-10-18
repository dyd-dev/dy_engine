#version 450

layout(push_constant) uniform Transform { vec4 value; } transform;
layout(location = 0) out vec2 uv;

void main()
{
    const vec2 positions[6] = vec2[](
        vec2(-1, -1), vec2(1, -1), vec2(1, 1),
        vec2(1, 1), vec2(-1, 1), vec2(-1, -1));
    vec2 p = positions[gl_VertexIndex];
    gl_Position = vec4(p * transform.value.zw + transform.value.xy, 0, 1);
    uv = p * vec2(0.5, -0.5) + vec2(0.5);
}
