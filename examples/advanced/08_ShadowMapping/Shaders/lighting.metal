#include <metal_stdlib>
using namespace metal;

bool2 greaterThanEqual(float2 a,float2 b) { return a>=b; }
bool3 greaterThanEqual(float3 a,float3 b) { return a>=b; }
bool2 lessThanEqual(float2 a,float2 b) { return a<=b; }

struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position [[position]]; float2 uv ; };
struct Output { float4 c0 [[color(0)]]; };
#define SAMPLE0(U) float4(image0.sample(imageSampler,U,level(0)))

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

fragment Output fragmentMain(Input input [[stage_in]], constant Params& p [[buffer(15)]], depth2d<float> image0 [[texture(0)]], sampler imageSampler [[sampler(8)]]) {
float2 uv=input.uv; Output o;

Surface s=traceScene(cameraRay(uv,p.sizeTime.xy));
float2 shadowUV=(s.position.xy+(6.0-s.position.z)*float2(0.5,0.3))/10.0+0.5;
float receiver=(6.0-s.position.z)/8.0;
float2 grid=float2(1.0),tile=float2(0.0),local=shadowUV;
float3 lightDirection=normalize(float3(0.5,0.3,1.0));float attenuation=1.0,region=0.0;
if(p.settings.w==1.0) {
    // Two camera-distance intervals use separately fitted orthographic projections.
    region=s.distance<p.extra.x ? 0.0 : 1.0;grid=float2(2.0,1.0);tile=float2(region,0.0);
    float extent=region<0.5 ? 12.0 : 28.0;
    local=(s.position.xy+(6.0-s.position.z)*float2(0.5,0.3)-float2(0.0,1.0))/extent+0.5;
}
if(p.settings.w==2.0) {
    float3 delta=s.position-float3(0.0,-2.0,p.extra.y),a=abs(delta);float2 q;
    if(a.x>=a.y && a.x>=a.z) { region=delta.x>=0.0 ? 0.0 : 1.0;q=delta.x>=0.0 ? float2(-delta.z,-delta.y)/max(a.x,0.001) : float2(delta.z,-delta.y)/max(a.x,0.001); }
    else if(a.y>=a.z) {region=delta.y>=0.0 ? 2.0 : 3.0;q=delta.y>=0.0 ? float2(delta.x,delta.z)/max(a.y,0.001) : float2(delta.x,-delta.z)/max(a.y,0.001);}
    else {region=delta.z>=0.0 ? 4.0 : 5.0;q=delta.z>=0.0 ? float2(delta.x,-delta.y)/max(a.z,0.001) : float2(-delta.x,-delta.y)/max(a.z,0.001);}
    grid=float2(3.0,2.0);tile=float2(region-floor(region/3.0)*3.0,floor(region/3.0));local=q*0.5+0.5;
    receiver=length(delta)/20.0;lightDirection=normalize(-delta);attenuation=3.0/(1.0+dot(delta,delta)*0.08);
}
shadowUV=(local+tile)/grid;
float visibility=1.0;
if(all(greaterThanEqual(local,float2(0.0))) && all(lessThanEqual(local,float2(1.0))) && receiver<=1.0) {
    visibility=0.0;
    int radius=int(p.settings.z);
    for(int y=-2;y<=2;y++) for(int x=-2;x<=2;x++) {
        if(abs(x)<=radius && abs(y)<=radius) {
            // Clamp PCF to this tile; neighboring cascade/cube-face depths are unrelated.
            float2 pixel=float2(1.0/1536.0,1.0/1024.0);
            float2 sampleUV=clamp(shadowUV+float2(float(x),float(y))*pixel,tile/grid+pixel*0.5,(tile+1.0)/grid-pixel*0.5);
            float stored=SAMPLE0(sampleUV).r;
            float receiverBias=p.settings.y*(1.0+6.0*(1.0-max(dot(s.normal,lightDirection),0.0)));
            visibility+=receiver-receiverBias<=stored ? 1.0 : 0.0;
        }
    }
    visibility/=float((radius*2+1)*(radius*2+1));
}
if(p.sizeTime.w==1.0) visibility=1.0;
float3 color=s.material<0.0 ? s.albedo : s.albedo*(0.1+visibility*max(dot(s.normal,lightDirection),0.0)*attenuation);
if(p.sizeTime.w==2.0) color=float3(SAMPLE0(uv).r);
if(p.sizeTime.w==3.0) color=float3(visibility);
if(p.sizeTime.w==4.0) color=(0.4+0.6*cos(float3(0.0,2.1,4.2)+region*1.7))*(0.25+visibility*0.75);
o.c0=float4(srgb(color*p.settings.x),1.0);

return o;
}
