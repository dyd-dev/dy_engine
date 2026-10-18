#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 inputUV;
layout(push_constant) uniform Draw { vec4 transform; } draw;
layout(location=0) out vec2 uv;
void main() { gl_Position=vec4(position*draw.transform.zw+draw.transform.xy,0,1); uv=inputUV; }
