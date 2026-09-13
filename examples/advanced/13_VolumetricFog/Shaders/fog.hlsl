struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
cbuffer Config:register(b15) { Params p; };
Texture2D<float4> image0:register(t0);
#define SAMPLE0(U) image0.SampleLevel(imageSampler,U,0)
SamplerState imageSampler:register(s8);

float3 eye() { return float3(0.0,-6.0,3.5); }
float3 cameraRay(float2 uv, float2 size) {
    float3 forward=normalize(float3(0.0,0.0,0.8)-eye());
    float3 right=normalize(cross(forward,float3(0.0,0.0,1.0)));
    float3 up=cross(right,forward);
    float2 q=(uv*2.0-1.0)*float2(size.x/size.y,-1.0);
    return normalize(forward+0.55*(q.x*right+q.y*up));
}

float SrgbChannel(float x) { x=max(x,0.0); return x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055; }
float3 srgb(float3 c) { return float3(SrgbChannel(c.r),SrgbChannel(c.g),SrgbChannel(c.b)); }
float3 aces(float3 c) { return clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),(float3)(0.0),(float3)(1.0)); }

Output main(Input input) {
float2 uv=input.uv; Output o;

float4 scene=SAMPLE0(uv);float3 rd=cameraRay(uv,p.sizeTime.xy);
int steps=int(p.extra.y);float distance=min(scene.a,18.0),dt=distance/float(steps);
float transmittance=1.0;float3 scattering=(float3)(0.0);
float g=0.35,mu=dot(rd,normalize(float3(-0.3,0.5,1.0)));
float phase=(1.0-g*g)/(12.5663706*pow(1.0+g*g-2.0*g*mu,1.5));
// Midpoint integration of Beer-Lambert extinction through procedural density.
for(int i=0;i<256;i++) {
    if(i>=steps) break;
    float3 pos=eye()+rd*((float(i)+0.5)*dt);
    float noise=0.65+0.35*sin(pos.x*2.1+p.sizeTime.z*0.3)*sin(pos.y*1.7)*sin(pos.z*2.9);
    float density=p.extra.x*exp(-max(pos.z,0.0)*p.settings.y)*noise;
    float stepTransmission=exp(-density*dt);
    scattering+=transmittance*(1.0-stepTransmission)*float3(0.5,0.7,1.0)*(0.35+phase*5.0);
    transmittance*=stepTransmission;
}
float3 color=scene.rgb*transmittance+scattering;
if(p.sizeTime.w==1.0) color=scene.rgb;
if(p.sizeTime.w==2.0) color=(float3)(transmittance);
if(p.sizeTime.w==3.0) color=scattering;
o.c0=float4(srgb(aces(color*p.settings.x)),1.0);

return o;
}
