#include <metal_stdlib>
using namespace metal;
struct Settings {float4 values;float4 whiteContrast;float4 liftSaturation;float4 gammaSharpen;float4 gainEnabled;float4 outputSize;};
struct ToneVertex {float4 position [[position]];float2 uv;};
vertex ToneVertex toneVertex(uint id [[vertex_id]]) {
    ToneVertex o;o.uv=float2((id<<1)&2,id&2);
    o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;
}
float3 OutputWhiteBalance(float3 color,constant Settings& settings)
{
    if(all(settings.whiteContrast.xyz==float3(1)))return color;
    float3 lms=float3(dot(color,float3(.390405,.549941,.00892632)),
        dot(color,float3(.0708416,.963172,.00135775)),dot(color,float3(.0231082,.128021,.936245)));
    lms*=settings.whiteContrast.xyz;
    return max(float3(dot(lms,float3(2.85847,-1.62879,-.024891)),
        dot(lms,float3(-.210182,1.1582,.000324281)),dot(lms,float3(-.041812,-.118169,1.06867))),float3(0));
}
float3 OutputColorGrade(float3 color,constant Settings& settings)
{
    if(settings.gainEnabled.w<.5)return color;
    color=pow(clamp(color*settings.gainEnabled.rgb+settings.liftSaturation.rgb,float3(0),float3(1)),
        float3(1)/max(settings.gammaSharpen.rgb,float3(.01)));
    color=(color-float3(.18))*settings.whiteContrast.w+float3(.18);
    float luminance=dot(color,float3(.2126,.7152,.0722));
    return clamp(mix(float3(luminance),color,settings.liftSaturation.w),float3(0),float3(1));
}
float3 OutputMapHdr(float3 hdr,constant Settings& settings)
{
    if(settings.values.w>.5)return clamp(hdr,float3(0),float3(1));
    hdr=min(max(hdr*settings.values.x,float3(0)),float3(16777216.0));
    if(settings.gainEnabled.w>.5)hdr=OutputWhiteBalance(hdr,settings);
    hdr=min(hdr,float3(16777216.0));
    return OutputColorGrade(hdr/(hdr+float3(1)),settings);
}
fragment float4 toneFragment(ToneVertex in [[stage_in]],texture2d<float> image [[texture(0)]],
    sampler imageSampler [[sampler(1)]],constant Settings& settings [[buffer(15)]])
{
    float4 hdr=image.sample(imageSampler,in.uv,level(0));
    if(settings.values.z>.5)
    {
        // RGBE 내보내기는 노출·색보정·선명도·출력 감마를 모두 우회한다.
        float3 radiance=clamp(hdr.rgb,float3(0),float3(65504));
        float peak=max(radiance.r,max(radiance.g,radiance.b));
        if(peak<=0)return float4(0);
        float exponent=clamp(ceil(log2(peak)),-127.0,127.0);
        return float4(radiance/exp2(exponent),(exponent+128)/255);
    }
    float3 mapped=OutputMapHdr(hdr.rgb,settings);
    if(settings.gammaSharpen.w>0 && settings.values.w<.5)
    {
        // 색보정 뒤 출력 픽셀 간격으로 선명도를 조절하며 이웃 색 범위를 넘기지 않는다.
        const float2 offsets[4]={float2(1,0),float2(-1,0),float2(0,1),float2(0,-1)};
        float3 minimum=mapped,maximum=mapped,blur=float3(0);float weightSum=0;
        float luminance=dot(mapped,float3(.2126,.7152,.0722));
        for(int i=0;i<4;++i)
        {
            float3 neighbor=OutputMapHdr(image.sample(imageSampler,in.uv+offsets[i]*settings.outputSize.xy,level(0)).rgb,settings);
            float weight=1/(1+abs(dot(neighbor,float3(.2126,.7152,.0722))-luminance)*16);
            blur+=neighbor*weight;weightSum+=weight;
            minimum=min(minimum,neighbor);maximum=max(maximum,neighbor);
        }
        mapped=clamp(mapped+settings.gammaSharpen.w*(mapped-blur/max(weightSum,.0001)),minimum,maximum);
    }
    if(settings.values.y>.5)mapped=pow(mapped,float3(1.0/2.2));
    return float4(mapped,hdr.a);
}
