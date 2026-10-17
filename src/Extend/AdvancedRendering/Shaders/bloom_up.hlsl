Texture2D<float4> baseImage:register(t0,space0);
Texture2D<float4> lowImage:register(t1,space0);
SamplerState imageSampler:register(s8,space0);
cbuffer Settings:register(b15,space0) {float4 texel;float4 unused;};
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    float3 blurred=0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
    {
        float weight=float(x==0?2:1)*float(y==0?2:1);
        blurred+=lowImage.SampleLevel(imageSampler,uv+float2(x,y)*texel.xy,0).rgb*weight;
    }
    float3 base=baseImage.SampleLevel(imageSampler,uv,0).rgb;
    return float4(lerp(base,blurred/16,.65),1);
}
