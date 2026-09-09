#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
layout(push_constant) uniform Transform { vec4 value; } transform;
layout(location=0) out vec2 outUv;
layout(location=1) out vec4 outColor;
void main() { gl_Position=vec4(position*transform.value.xy+transform.value.zw,0,1); outUv=uv; outColor=color; }
