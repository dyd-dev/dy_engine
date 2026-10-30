struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
cbuffer Config:register(b15) { Params p; };
Texture2D<float4> image0:register(t0);
Texture2D<float4> image1:register(t1);
Texture2D<float4> image2:register(t2);
SamplerState imageSampler:register(s8);

Output main(Input input) {
float2 uv=input.uv; Output o;

    float4 current=image0.SampleLevel(imageSampler,uv,0), motion=image1.SampleLevel(imageSampler,uv,0);
float2 historyUV=uv+motion.xy+(p.jitter.xy-p.jitter.zw)/p.sizeTime.xy;
    float4 history=image2.SampleLevel(imageSampler,historyUV,0);
float3 lower=current.rgb,upper=current.rgb;
for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
    float3 c=image0.SampleLevel(imageSampler,uv+float2(float(x),float(y))/p.sizeTime.xy,0).rgb;
    lower=min(lower,c);upper=max(upper,c);
}
float weight=p.settings.w;
    if(p.extra.x<0.5 || abs(history.a-current.a)>0.1 || any(historyUV<(float2)(0.0)) || any(historyUV>(float2)(1.0))) weight=0.0;
    history.rgb=clamp(history.rgb,lower,upper);
o.c0=float4(lerp(current.rgb,history.rgb,weight),current.a);

return o;
}
