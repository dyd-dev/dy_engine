#version 450
layout(location=0) out vec4 color;
void main() {
    const vec2 positions[3] = vec2[](vec2(-0.7,-0.6), vec2(0.7,-0.6), vec2(0,0.7));
    const vec4 colors[3] = vec4[](vec4(1,0.2,0.15,1), vec4(0.2,1,0.3,1), vec4(0.2,0.4,1,1));
    gl_Position = vec4(positions[gl_VertexIndex],0,1);
    color = colors[gl_VertexIndex];
}
