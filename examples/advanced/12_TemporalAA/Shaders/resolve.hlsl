bool2 lessThan(float2 a,float2 b) { return a<b; }
bool3 lessThan(float3 a,float3 b) { return a<b; }
bool2 greaterThan(float2 a,float2 b) { return a>b; }

struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
cbuffer Config:register(b15) { Params p; };
Texture2D<float4> image0:register(t0);
#define SAMPLE0(U) image0.SampleLevel(imageSampler,U,0)
Texture2D<float4> image1:register(t1);
#define SAMPLE1(U) image1.SampleLevel(imageSampler,U,0)
Texture2D<float4> image2:register(t2);
#define SAMPLE2(U) image2.SampleLevel(imageSampler,U,0)
SamplerState imageSampler:register(s8);

Output main(Input input) {
float2 uv=input.uv; Output o;

float4 current=SAMPLE0(uv), motion=SAMPLE1(uv);
float2 historyUV=uv+motion.xy+(p.jitter.xy-p.jitter.zw)/p.sizeTime.xy;
float4 history=SAMPLE2(historyUV);
float3 lower=current.rgb,upper=current.rgb;
for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
    float3 c=SAMPLE0(uv+float2(float(x),float(y))/p.sizeTime.xy).rgb;
    lower=min(lower,c);upper=max(upper,c);
}
float weight=p.settings.w;
if(p.extra.x<0.5 || abs(history.a-current.a)>0.1 || any(lessThan(historyUV,(float2)(0.0))) || any(greaterThan(historyUV,(float2)(1.0)))) weight=0.0;
if(p.sizeTime.w!=2.0) history.rgb=clamp(history.rgb,lower,upper);
if(p.sizeTime.w==1.0) weight=0.0;
o.c0=float4(lerp(current.rgb,history.rgb,weight),current.a);

return o;
}
