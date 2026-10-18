Texture2D hdrImage:register(t0,space0);
SamplerState hdrSampler:register(s1,space0);
cbuffer Settings:register(b15,space0) {float4 settings;float4 whiteContrast;float4 liftSaturation;float4 gammaSharpen;float4 gainEnabled;float4 outputSize;};
float3 OutputWhiteBalance(float3 color)
{
    if(all(whiteContrast.xyz==float3(1,1,1)))return color;
    float3 lms=float3(dot(color,float3(.390405,.549941,.00892632)),
        dot(color,float3(.0708416,.963172,.00135775)),dot(color,float3(.0231082,.128021,.936245)));
    lms*=whiteContrast.xyz;
    return max(float3(dot(lms,float3(2.85847,-1.62879,-.024891)),
        dot(lms,float3(-.210182,1.1582,.000324281)),dot(lms,float3(-.041812,-.118169,1.06867))),float3(0,0,0));
}
float3 OutputColorGrade(float3 color)
{
    if(gainEnabled.w<.5)return color;
    color=pow(clamp(color*gainEnabled.rgb+liftSaturation.rgb,float3(0,0,0),float3(1,1,1)),
        float3(1,1,1)/max(gammaSharpen.rgb,float3(.01,.01,.01)));
    color=(color-float3(.18,.18,.18))*whiteContrast.w+float3(.18,.18,.18);
    float luminance=dot(color,float3(.2126,.7152,.0722));
    return clamp(lerp(float3(luminance,luminance,luminance),color,liftSaturation.w),float3(0,0,0),float3(1,1,1));
}
float3 OutputMapHdr(float3 hdr)
{
    if(settings.w>.5)return clamp(hdr,float3(0,0,0),float3(1,1,1));
    hdr=min(max(hdr*settings.x,float3(0,0,0)),float3(16777216.0,16777216.0,16777216.0));
    if(gainEnabled.w>.5)hdr=OutputWhiteBalance(hdr);
    hdr=min(hdr,float3(16777216.0,16777216.0,16777216.0));
    return OutputColorGrade(hdr/(hdr+float3(1,1,1)));
}
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    float4 hdr=hdrImage.SampleLevel(hdrSampler,uv,0);
    if(settings.z>.5)
    {
        // RGBE 내보내기는 노출·색보정·선명도·출력 감마를 모두 우회한다.
        float3 radiance=clamp(hdr.rgb,float3(0,0,0),float3(65504,65504,65504));
        float peak=max(radiance.r,max(radiance.g,radiance.b));
        if(peak<=0)return float4(0,0,0,0);
        float exponent=clamp(ceil(log2(peak)),-127.0,127.0);
        return float4(radiance/exp2(exponent),(exponent+128)/255);
    }
    float3 mapped=OutputMapHdr(hdr.rgb);
    if(gammaSharpen.w>0 && settings.w<.5)
    {
        // 색보정 뒤 출력 픽셀 간격으로 선명도를 조절하며 이웃 색 범위를 넘기지 않는다.
        const float2 offsets[4]={float2(1,0),float2(-1,0),float2(0,1),float2(0,-1)};
        float3 minimum=mapped,maximum=mapped,blur=float3(0,0,0);float weightSum=0;
        float luminance=dot(mapped,float3(.2126,.7152,.0722));
        for(int i=0;i<4;++i)
        {
            float3 neighbor=OutputMapHdr(hdrImage.SampleLevel(hdrSampler,uv+offsets[i]*outputSize.xy,0).rgb);
            float weight=1/(1+abs(dot(neighbor,float3(.2126,.7152,.0722))-luminance)*16);
            blur+=neighbor*weight;weightSum+=weight;
            minimum=min(minimum,neighbor);maximum=max(maximum,neighbor);
        }
        mapped=clamp(mapped+gammaSharpen.w*(mapped-blur/max(weightSum,.0001)),minimum,maximum);
    }
    if(settings.y>.5)mapped=pow(mapped,float3(1.0/2.2,1.0/2.2,1.0/2.2));
    return float4(mapped,hdr.a);
}
