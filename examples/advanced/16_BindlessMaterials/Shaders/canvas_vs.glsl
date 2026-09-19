#version 450
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(push_constant) uniform Constants { vec4 transform; vec4 parameters; } constants;
layout(location = 0) out vec2 outUV;
void main()
{
    gl_Position = vec4(position * constants.transform.xy + constants.transform.zw, 0, 1);
    outUV = uv;
}
