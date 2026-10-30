struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
cbuffer Config:register(b15) { Params p; };
Texture2D<float4> image0:register(t0);
#define SAMPLE0(U) image0.SampleLevel(imageSampler,U,0)
SamplerState imageSampler:register(s8);

Output main(Input input) {
float2 uv=input.uv; Output o;

float3 sum=(float3)(0.0);float weightSum=0.0;
for(int i=-6;i<=6;i++) {
    float w=exp(-float(i*i)/12.0);
    sum+=SAMPLE0(uv+float2(0.0,float(i)/p.sizeTime.y)).rgb*w;weightSum+=w;
}
o.c0=float4(sum/weightSum,1.0);

return o;
}
