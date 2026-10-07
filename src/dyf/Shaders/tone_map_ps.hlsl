Texture2D hdrImage:register(t0,space0);
SamplerState hdrSampler:register(s1,space0);
cbuffer Settings:register(b15,space0) {float4 settings;};
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target {
    float4 hdr=hdrImage.Sample(hdrSampler,uv);
    // At 2^24, x + 1 rounds to x in float; brighter values already map to 1.
    float3 mapped=min(max(hdr.rgb*settings.x,0),16777216.0);
    mapped=mapped/(mapped+1);
    if(settings.y>.5)mapped=pow(mapped,1./2.2);
    return float4(mapped,hdr.a);
}
