#include <metal_stdlib>
using namespace metal;
struct Input {
    float4 position [[position]];
    float2 uv ;
};
struct Output {
    float depth [[depth(any)]];
};
fragment Output fragmentMain(Input input [[stage_in]]) {
    float2 uv=input.uv;
    Output o;
    float3 origin=float3((uv-0.5)*10.0,6.0),rd=normalize(float3(-0.5,-0.3,-1.0));
    float closest=20.0;
    if(rd.z<-0.001) closest=min(closest,-origin.z/rd.z);
    for(int i=0; i<3; i++) {
        float3 oc=origin-(float3(float(i-1)*1.9,0.0,0.9));
        float b=dot(oc,rd);
        float h=b*b-dot(oc,oc)+(0.88)*(0.88);
        float t=h>0.0 ? -b-sqrt(h) : -1.0;
        if(t>0.0) closest=min(closest,t);
    }
    float3 hit=origin+rd*closest;
    o.depth=clamp((6.0-hit.z)/8.0,0.0,1.0);
    return o;
}
