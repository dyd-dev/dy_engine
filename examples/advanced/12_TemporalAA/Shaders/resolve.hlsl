struct Params {
    float2 screenSize;
    float time;
    float previousTime;
    float2 jitter;
    float2 previousJitter;
    float historyWeight;
    float historyValid;
};
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
cbuffer Config:register(b15) { Params p; };
Texture2D<float4> currentColor:register(t0);
Texture2D<float4> motionVectors:register(t1);
Texture2D<float4> previousColor:register(t2);
SamplerState imageSampler:register(s8);

Output main(Input input) {
float2 uv=input.uv; Output o;

    float4 current=currentColor.SampleLevel(imageSampler,uv,0), motion=motionVectors.SampleLevel(imageSampler,uv,0);
float2 historyUV=uv+motion.xy+(p.jitter-p.previousJitter)/p.screenSize;
    float4 history=previousColor.SampleLevel(imageSampler,historyUV,0);
float3 lower=current.rgb,upper=current.rgb;
for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
    float3 c=currentColor.SampleLevel(imageSampler,uv+float2(float(x),float(y))/p.screenSize,0).rgb;
    lower=min(lower,c);upper=max(upper,c);
}
float weight=p.historyWeight;
    if(p.historyValid<0.5 || abs(history.a-current.a)>0.1 || any(historyUV<(float2)(0.0)) || any(historyUV>(float2)(1.0))) weight=0.0;
    history.rgb=clamp(history.rgb,lower,upper);
o.c0=float4(lerp(current.rgb,history.rgb,weight),current.a);

return o;
}
