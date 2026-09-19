#version 450
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 instanceTransform;
layout(location = 2) in vec4 instanceColor;
layout(location = 0) out vec4 color;
void main()
{
    gl_Position = vec4(position * instanceTransform.zw + instanceTransform.xy, 0.0, 1.0);
    color = instanceColor;
}
