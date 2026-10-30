struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
cbuffer Config:register(b15) { Params p; };
Texture2D<float4> image0:register(t0);
Texture2D<float4> image1:register(t1);
SamplerState imageSampler:register(s8);

Output main(Input input) {
float2 uv=input.uv; Output o;

    float3 hdr=image0.SampleLevel(imageSampler,uv,0).rgb, bloom=image1.SampleLevel(imageSampler,uv,0).rgb;
    hdr+=bloom*p.settings.z;
hdr*=p.settings.x;
    float3 color=clamp((hdr*(2.51*hdr+0.03))/(hdr*(2.43*hdr+0.59)+0.14),(float3)(0.0),(float3)(1.0));
    for(int channel=0;channel<3;channel++) {
        float x=max(color[channel],0.0);
        color[channel]=x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055;
    }
    o.c0=float4(color,1.0);

return o;
}
