#include <metal_stdlib>
using namespace metal;

struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };

float3 lightPosition(int i,float time) {
    float f=float(i); return float3(sin(f*2.17+time*0.17)*4.0,cos(f*1.37+time*0.11)*3.0,0.5+fract(f*0.319)*2.5);
}

kernel void computeMain(uint3 id [[thread_position_in_grid]],constant Params& p [[buffer(15)]],device uint* indices [[buffer(0)]]) {

uint cluster=id.x;if(cluster>=2048u) return;
uint x=cluster%16u,y=(cluster/16u)%16u,z=cluster/256u;
float3 lower=float3(-6.0,-6.0,0.0)+float3(float(x),float(y),float(z))*float3(0.75,0.75,0.5);
float3 upper=lower+float3(0.75,0.75,0.5);uint count=0u,base=cluster*65u;
for(int i=0;i<int(p.extra.z);i++) {
    float3 position=lightPosition(i,p.sizeTime.z),closest=clamp(position,lower,upper);
    if(dot(position-closest,position-closest)<=2.4*2.4) indices[base+1u+count++]=uint(i);
}
indices[base]=count;

}
