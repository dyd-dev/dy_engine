Texture2D<float4> image:register(t0,space0);
SamplerState imageSampler:register(s8,space0);
cbuffer Settings:register(b15,space0) {float4 texelThreshold;float4 mode;};
static const float2 offsets[13]={
    float2(-2,-2),float2(0,-2),float2(2,-2),float2(-2,0),float2(0,0),float2(2,0),
    float2(-2,2),float2(0,2),float2(2,2),float2(-1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
static const float weights[13]={.03125,.0625,.03125,.0625,.125,.0625,.03125,.0625,.03125,.125,.125,.125,.125};
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    float3 filtered=0;
    float total=0;
    for(int i=0;i<13;++i)
    {
        float3 sampleColor=clamp(image.SampleLevel(imageSampler,uv+offsets[i]*texelThreshold.xy,0).rgb,0,64000);
        float weight=weights[i];
        if(mode.x>.5) weight/=1+dot(sampleColor,float3(.2126,.7152,.0722));
        filtered+=sampleColor*weight;total+=weight;
    }
    filtered/=max(total,1e-6);
    if(mode.x>.5)
    {
        float brightness=max(max(filtered.r,filtered.g),filtered.b);
        float threshold=texelThreshold.z,knee=texelThreshold.w;
        float soft=clamp(brightness-threshold+knee,0,2*knee);
        soft=soft*soft/max(4*knee,1e-5);
        filtered*=max(soft,brightness-threshold)/max(brightness,1e-5);
    }
    return float4(filtered,1);
}
