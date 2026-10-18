#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec4 inputColor;
layout(push_constant) uniform Draw { vec4 transform; } draw;
layout(location=0) out vec4 color;
void main() { gl_Position=vec4(position.xy*draw.transform.zw+draw.transform.xy,position.z,1); color=inputColor; }
