#version 450
layout(local_size_x=64,local_size_y=1,local_size_z=1) in;
struct Vertex { vec4 position; vec4 color; };
layout(set=0,binding=0,std430) buffer Output { Vertex vertices[]; } outputVertices;
layout(push_constant) uniform Parameters { vec4 value; } parameters;
void main() {
    uint id=gl_GlobalInvocationID.x; if(id>=3) return;
    const vec2 positions[3]=vec2[](vec2(-0.7,-0.6),vec2(0.7,-0.6),vec2(0,0.7));
    const vec3 colors[3]=vec3[](vec3(1,0.2,0.15),vec3(0.2,1,0.3),vec3(0.2,0.4,1));
    float angle=parameters.value.x*0.4, c=cos(angle), s=sin(angle);
    vec2 p=positions[id];
    outputVertices.vertices[id].position=vec4(c*p.x-s*p.y,s*p.x+c*p.y,0,1);
    outputVertices.vertices[id].color=vec4(colors[id]*(0.75+0.25*sin(parameters.value.x+float(id))),1);
}
