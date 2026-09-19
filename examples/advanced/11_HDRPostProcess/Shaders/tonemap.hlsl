struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
cbuffer Config:register(b15) { Params p; };
Texture2D<float4> image0:register(t0);
#define SAMPLE0(U) image0.SampleLevel(imageSampler,U,0)
Texture2D<float4> image1:register(t1);
#define SAMPLE1(U) image1.SampleLevel(imageSampler,U,0)
SamplerState imageSampler:register(s8);

float SrgbChannel(float x) { x=max(x,0.0); return x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055; }
float3 srgb(float3 c) { return float3(SrgbChannel(c.r),SrgbChannel(c.g),SrgbChannel(c.b)); }
float3 aces(float3 c) { return clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),(float3)(0.0),(float3)(1.0)); }

Output main(Input input) {
float2 uv=input.uv; Output o;

float3 hdr=SAMPLE0(uv).rgb, bloom=SAMPLE1(uv).rgb;
if(p.sizeTime.w!=3.0) hdr+=bloom*p.settings.z;
hdr*=p.settings.x;
float3 color=aces(hdr);
if(p.sizeTime.w==1.0) color=hdr/(1.0+hdr);
if(p.sizeTime.w==2.0) color=clamp(hdr,(float3)(0.0),(float3)(1.0));
if(p.sizeTime.w==4.0) color=aces(bloom*p.settings.x);
o.c0=float4(srgb(color),1.0);

return o;
}
