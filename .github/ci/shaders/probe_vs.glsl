#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 inUv;
layout(push_constant) uniform Constants { vec4 color; vec4 parameters; } constants;
layout(location = 0) out vec2 uv;
void main()
{
    gl_Position = vec4(position.xy * constants.parameters.x, constants.parameters.y, 1.0);
    uv = inUv;
}
