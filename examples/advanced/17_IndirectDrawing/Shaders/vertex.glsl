#version 450
layout(push_constant) uniform Display { vec4 display; };
layout(location=0) out vec3 color;
void main()
{
    const vec2 vertices[3] = vec2[3](vec2(-0.075, -0.065), vec2(0.075, -0.065), vec2(0.0, 0.08));
    uint instance = uint(gl_InstanceIndex);
    vec2 center = vec2((float(instance % 8u) - 3.5) * 0.18 + mix(-0.78, 0.78, display.z),
        (float(instance / 8u) - 3.5) * 0.20);
    gl_Position = vec4((center + vertices[gl_VertexIndex]) * display.xy, 0.0, 1.0);
    color = mix(vec3(0.12, 0.80, 0.95), vec3(1.0, 0.46, 0.12), display.z)
        * (0.65 + 0.35 * float(instance % 8u) / 7.0);
}
