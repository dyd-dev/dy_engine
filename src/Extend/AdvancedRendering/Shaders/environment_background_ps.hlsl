Texture2D<float> depthTexture : register(t0);
Texture2D<float4> radianceTexture : register(t6);
SamplerState environmentSampler : register(s7);
cbuffer FrameConstants : register(b9)
{
    column_major float4x4 inverseViewProjection;
    float4 cameraPosition;
    float4 sh[9];
    float4 settings;
    float4 screen;
    float4 probePosition;
    float4 probeBoxMin;
    float4 probeBoxMax;
    float4 specularColor;
    column_major float4x4 backgroundRayToWorld;
};
float3 SampleRadiance(float2 coordinate)
{
    uint width,height;radianceTexture.GetDimensions(width,height);
    int2 size=int2(width,height);float2 pixel=coordinate*size-0.5,weight=frac(pixel);
    int2 low=int2(floor(pixel)),high=low+1;
    low.x=(low.x%size.x+size.x)%size.x;high.x=(high.x%size.x+size.x)%size.x;
    low.y=clamp(low.y,0,size.y-1);high.y=clamp(high.y,0,size.y-1);
    return lerp(lerp(radianceTexture.Load(int3(low,0)),radianceTexture.Load(int3(high.x,low.y,0)),weight.x),
        lerp(radianceTexture.Load(int3(low.x,high.y,0)),radianceTexture.Load(int3(high,0)),weight.x),weight.y).rgb;
}
float4 main(float4 fragment:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    if(depthTexture.Load(int3(int2(fragment.xy),0))<1) discard;
    float2 coordinate=fragment.xy*screen.xy;
    float2 clip=coordinate*float2(2,-2)+float2(-1,1);
    float4 a=mul(backgroundRayToWorld,float4(clip,0,1)),b=mul(backgroundRayToWorld,float4(clip,1,1));
    float3 direction=normalize(b.xyz*a.w-a.xyz*b.w);
    float c=cos(settings.z),s=sin(settings.z);
    direction.xy=float2(c*direction.x+s*direction.y,-s*direction.x+c*direction.y);
    float2 skyUV=float2(atan2(direction.y,direction.x)*0.15915494309189535+0.5,acos(clamp(direction.z,-1.0,1.0))*0.3183098861837907);
    return float4(clamp(SampleRadiance(skyUV)*settings.y,0,65504),1);
}
