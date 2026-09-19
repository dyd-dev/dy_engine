#version 450
layout(location = 0) in vec2 position;
layout(push_constant) uniform Constants { vec4 transform; vec4 tint; } constants;
layout(location = 0) out vec4 color;
void main()
{
    gl_Position = vec4(position * constants.transform.xy + constants.transform.zw, 0, 1);
    color = constants.tint;
}
