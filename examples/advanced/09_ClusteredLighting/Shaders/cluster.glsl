#version 450
struct Params {
    vec2 screenSize;
    float time;
    uint lightCount;
    float exposure;
};
layout(push_constant) uniform Config { Params p; };
layout(set=0,binding=0,std430) buffer LightLists { uint indices[]; };
layout(local_size_x=64) in;

void main() {
    uvec3 id=gl_GlobalInvocationID;

    uint cluster=id.x;
    uint x=cluster%16u,y=(cluster/16u)%16u,z=cluster/256u;
    vec3 lower=vec3(-6.0,-6.0,0.0)+vec3(float(x),float(y),float(z))*vec3(0.75,0.75,0.5);
    vec3 upper=lower+vec3(0.75,0.75,0.5);uint count=0u,base=cluster*65u;
    for(int i=0;i<int(p.lightCount);i++) {
        float f=float(i); vec3 position=vec3(sin(f*2.17+p.time*0.17)*4.0,cos(f*1.37+p.time*0.11)*3.0,0.5+fract(f*0.319)*2.5),closest=clamp(position,lower,upper);
        if(dot(position-closest,position-closest)<=2.4*2.4) indices[base+1u+count++]=uint(i);
    }
    indices[base]=count;
}
