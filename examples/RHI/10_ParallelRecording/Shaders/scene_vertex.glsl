#version 450

layout(push_constant) uniform Parameters { vec4 value; } parameters;
layout(location = 0) out vec4 vertexColor;

void main()
{
    const vec2 positions[3] = vec2[](vec2(-0.7, -0.6), vec2(0.7, -0.6), vec2(0, 0.7));
    const vec3 colors[3] = vec3[](vec3(1, 0.2, 0.15), vec3(0.2, 1, 0.3), vec3(0.2, 0.4, 1));
    vec2 p = positions[gl_VertexIndex];
    float c = cos(parameters.value.x), s = sin(parameters.value.x);
    gl_Position = vec4(c * p.x - s * p.y, s * p.x + c * p.y, 0, 1);
    vertexColor = vec4(colors[gl_VertexIndex], 1);
}
