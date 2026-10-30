#include <metal_stdlib>
using namespace metal;

bool2 lessThan(float2 a,float2 b) { return a<b; }
bool3 lessThan(float3 a,float3 b) { return a<b; }

bool2 greaterThanEqual(float2 a,float2 b) { return a>=b; }
bool3 greaterThanEqual(float3 a,float3 b) { return a>=b; }

struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float4 c0 [[color(0)]]; };

// Analytic surfaces keep this lighting exercise independent of model assets.
struct Surface { float3 position; float3 normal; float3 albedo; float distance; float material; };
float3 eye() { return float3(0.0,-6.0,3.5); }
float3 cameraRay(float2 uv, float2 size) {
    float3 forward=normalize(float3(0.0,0.0,0.8)-eye());
    float3 right=normalize(cross(forward,float3(0.0,0.0,1.0)));
    float3 up=cross(right,forward);
    float2 q=(uv*2.0-1.0)*float2(size.x/size.y,-1.0);
    return normalize(forward+0.55*(q.x*right+q.y*up));
}
float sphereHit(float3 origin,float3 direction,float3 center,float radius) {
    float3 oc=origin-center; float b=dot(oc,direction);
    float h=b*b-dot(oc,oc)+radius*radius;
    return h>0.0 ? -b-sqrt(h) : -1.0;
}
Surface traceScene(float3 rd) {
    Surface s; s.distance=40.0; s.position=eye()+rd*40.0;
    s.normal=float3(0.0,0.0,1.0); s.albedo=float3(0.04,0.08,0.14); s.material=-1.0;
    if(rd.z<-0.001) {
        float t=-eye().z/rd.z;
        if(t>0.0 && t<24.0) {
            s.distance=t; s.position=eye()+rd*t;
            float checker=fract((floor(s.position.x)+floor(s.position.y))*0.5)*2.0;
            s.albedo=mix(float3(0.16),float3(0.32),checker); s.material=0.0;
        }
    }
    for(int i=0;i<3;i++) {
        float3 center=float3(float(i-1)*1.9,0.0,0.9);
        float t=sphereHit(eye(),rd,center,0.88);
        if(t>0.0 && t<s.distance) {
            s.distance=t; s.position=eye()+rd*t; s.normal=normalize(s.position-center);
            s.albedo=i==0 ? float3(0.9,0.15,0.06) : (i==1 ? float3(0.15,0.65,0.9) : float3(0.95,0.64,0.18));
            s.material=float(i+1);
        }
    }
    return s;
}
float SrgbChannel(float x) { x=max(x,0.0); return x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055; }
float3 srgb(float3 c) { return float3(SrgbChannel(c.r),SrgbChannel(c.g),SrgbChannel(c.b)); }
float3 aces(float3 c) { return clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),float3(0.0),float3(1.0)); }

float3 lightPosition(int i,float time) {
    float f=float(i); return float3(sin(f*2.17+time*0.17)*4.0,cos(f*1.37+time*0.11)*3.0,0.5+fract(f*0.319)*2.5);
}
float3 lightColor(int i) { return 0.4+0.6*cos(float3(0.0,2.1,4.2)+float(i)*0.71); }
float3 pointLight(Surface s,int i,float time) {
    float3 delta=lightPosition(i,time)-s.position; float d=length(delta), radius=2.4;
    float attenuation=pow(max(1.0-d/radius,0.0),2.0);
    return s.albedo*lightColor(i)*max(dot(s.normal,delta/max(d,0.001)),0.0)*attenuation*1.8;
}

fragment Output fragmentMain(Input input [[stage_in]], constant Params& p [[buffer(15)]], const device uint* indices [[buffer(0)]]) {
float2 uv=input.uv; Output o;

Surface s=traceScene(cameraRay(uv,p.sizeTime.xy)); float3 color=s.albedo;
if(s.material>=0.0) {
    // World-space clusters cover [-6,6] x [-6,6] x [0,4].
    float3 normalized=(s.position-float3(-6.0,-6.0,0.0))/float3(12.0,12.0,4.0);
    int3 cell=int3(clamp(floor(normalized*float3(16.0,16.0,8.0)),float3(0.0),float3(15.0,15.0,7.0)));
    uint cluster=uint(cell.x+16*(cell.y+16*cell.z)), base=cluster*65u;
    uint count=indices[base];color=s.albedo*0.04;
    bool outside=any(lessThan(normalized,float3(0.0))) || any(greaterThanEqual(normalized,float3(1.0)));
    if(p.sizeTime.w==1.0 || outside) {
        for(int i=0;i<int(p.extra.z);i++) color+=pointLight(s,i,p.sizeTime.z);
    } else {
        for(uint j=0u;j<count;j++) color+=pointLight(s,int(indices[base+1u+j]),p.sizeTime.z);
    }
    if(p.sizeTime.w==2.0) color=float3(float(count)/max(p.extra.z,1.0),0.1,1.0-float(count)/max(p.extra.z,1.0));
}
o.c0=float4(srgb(aces(color*p.settings.x)),1.0);

return o;
}
