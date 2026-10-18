#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec4 inputColor;
layout(push_constant) uniform Draw { vec4 transform; } draw;
layout(location=0) out vec4 color;
void main() { gl_Position=vec4(position*draw.transform.zw+draw.transform.xy,0,1); color=inputColor; }
