#version 450
layout(set=0,binding=0,std140) uniform Globals { vec4 tint; } globals;
layout(location=0) in vec4 color;
layout(location=0) out vec4 result;
void main() { result=color*globals.tint; }
