#include <metal_stdlib>
using namespace metal;

struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float depth [[depth(any)]]; };

float sphereHit(float3 origin,float3 direction,float3 center,float radius) {
    float3 oc=origin-center; float b=dot(oc,direction);
    float h=b*b-dot(oc,oc)+radius*radius;
    return h>0.0 ? -b-sqrt(h) : -1.0;
}

fragment Output fragmentMain(Input input [[stage_in]], constant Params& p [[buffer(15)]]) {
float2 uv=input.uv; Output o;

// Each atlas tile has its own light projection; fragment depth stores comparable distance.
float3 origin=float3((uv-0.5)*10.0,6.0),rd=normalize(float3(-0.5,-0.3,-1.0));
if(p.settings.w==1.0) {
    float cascade=floor(uv.x*2.0);float2 local=float2(fract(uv.x*2.0),uv.y);
    float extent=cascade<0.5 ? 12.0 : 28.0;
    origin=float3((local-0.5)*extent+float2(0.0,1.0),6.0);
}
if(p.settings.w==2.0) {
    float2 tile=floor(uv*float2(3.0,2.0)),q=fract(uv*float2(3.0,2.0))*2.0-1.0;
    int face=int(tile.x+tile.y*3.0);
    if(face==0) rd=float3(1.0,-q.y,-q.x);
    if(face==1) rd=float3(-1.0,-q.y,q.x);
    if(face==2) rd=float3(q.x,1.0,q.y);
    if(face==3) rd=float3(q.x,-1.0,-q.y);
    if(face==4) rd=float3(q.x,-q.y,1.0);
    if(face==5) rd=float3(-q.x,-q.y,-1.0);
    origin=float3(0.0,-2.0,p.extra.y);rd=normalize(rd);
}
float closest=20.0;
if(rd.z<-0.001) closest=min(closest,-origin.z/rd.z);
for(int i=0;i<3;i++) {
    float t=sphereHit(origin,rd,float3(float(i-1)*1.9,0.0,0.9),0.88);
    if(t>0.0) closest=min(closest,t);
}
float3 hit=origin+rd*closest;
o.depth=p.settings.w==2.0 ? closest/20.0 : clamp((6.0-hit.z)/8.0,0.0,1.0);

return o;
}
