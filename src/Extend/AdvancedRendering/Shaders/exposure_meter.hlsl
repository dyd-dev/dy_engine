Texture2D<float4> sourceImage:register(t0,space0);
Texture2D<float4> previousImage:register(t1,space0);
SamplerState imageSampler:register(s8,space0);
cbuffer Settings:register(b15,space0) {float4 operation;};
static const float minimumLogLuminance=-13.28771238; // log2(0.0001)
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    if(operation.x>1.5)
    {
        float target=sourceImage.Load(int3(0,0,0)).r;
        if(!isfinite(target)) target=minimumLogLuminance;
        float adapted=target;
        if(operation.y>.5)
        {
            float previous=previousImage.Load(int3(0,0,0)).r;
            if(isfinite(previous)) adapted=lerp(previous,target,operation.z);
        }
        return float4(adapted,target,0,1);
    }
    uint width,height;sourceImage.GetDimensions(width,height);
    uint2 origin=uint2(position.xy)*4;
    float weightedLog=0,weight=0;
    for(uint y=0;y<4;++y) for(uint x=0;x<4;++x)
    {
        uint2 pixel=origin+uint2(x,y);
        if(any(pixel>=uint2(width,height))) continue;
        float4 value=sourceImage.Load(int3(pixel,0));
        if(operation.x<.5)
        {
            if(!all(isfinite(value.rgb))) continue;
            float luminance=dot(clamp(value.rgb,0,65504),float3(.2126,.7152,.0722));
            weightedLog+=log2(max(luminance,.0001));weight+=1;
        }
        else
        {
            weightedLog+=value.r*value.g;weight+=value.g;
        }
    }
    return float4(weight>0?weightedLog/weight:minimumLogLuminance,weight/16,0,1);
}
