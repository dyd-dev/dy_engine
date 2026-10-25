#include <metal_stdlib>
using namespace metal;

struct Params {
    float2 screenSize;
    float exposure;
    float threshold;
    float bloomStrength;
    float emissiveIntensity;
};
struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float4 c0 [[color(0)]]; };

// Analytic surfaces keep this lighting exercise independent of model assets.
struct Surface { float3 position; float3 normal; float3 albedo; float distance; float material; };

fragment Output fragmentMain(Input input [[stage_in]], constant Params& p [[buffer(15)]]) {
float2 uv=input.uv; Output o;

    const float3 cameraPosition=float3(0.0,-6.0,3.5);

    float3 forward=normalize(float3(0.0,0.0,0.8)-cameraPosition);
    float3 right=normalize(cross(forward,float3(0.0,0.0,1.0)));
    float3 up=cross(right,forward);
    float2 cameraCoord=(uv*2.0-1.0)*float2(p.screenSize.x/p.screenSize.y,-1.0);
    float3 rd=normalize(forward+0.55*(cameraCoord.x*right+cameraCoord.y*up));

    Surface s; s.distance=40.0; s.position=cameraPosition+rd*40.0;
    s.normal=float3(0.0,0.0,1.0); s.albedo=float3(0.04,0.08,0.14); s.material=-1.0;
    if(rd.z<-0.001) {
        float t=-cameraPosition.z/rd.z;
        if(t>0.0 && t<24.0) {
            s.distance=t; s.position=cameraPosition+rd*t;
            float checker=fract((floor(s.position.x)+floor(s.position.y))*0.5)*2.0;
            s.albedo=mix(float3(0.16),float3(0.32),checker); s.material=0.0;
        }
    }
    for(int i=0;i<3;i++) {
        float3 center=float3(float(i-1)*1.9,0.0,0.9);
        float3 oc=cameraPosition-center; float b=dot(oc,rd);
        float h=b*b-dot(oc,oc)+0.88*0.88;
        float t=h>0.0 ? -b-sqrt(h) : -1.0;
        if(t>0.0 && t<s.distance) {
            s.distance=t; s.position=cameraPosition+rd*t; s.normal=normalize(s.position-center);
            s.albedo=i==0 ? float3(0.9,0.15,0.06) : (i==1 ? float3(0.15,0.65,0.9) : float3(0.95,0.64,0.18));
            s.material=float(i+1);
        }
    }
float3 color=s.albedo;
if(s.material>=0.0) color=s.albedo*(0.07+max(dot(s.normal,normalize(float3(-0.3,-0.5,1.0))),0.0))*2.0;
if(s.material>1.5 && s.material<2.5) color+=float3(0.1,0.55,1.0)*p.emissiveIntensity;
float2 q=uv-float2(0.78,0.22);
color+=float3(1.0,0.55,0.12)*exp(-dot(q,q)*1600.0)*p.emissiveIntensity*2.0;
o.c0=float4(color,1.0);

return o;
}
