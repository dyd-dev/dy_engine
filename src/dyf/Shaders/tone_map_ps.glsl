#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform texture2D hdrImage;
layout(set=0,binding=1) uniform sampler hdrSampler;
layout(push_constant) uniform Settings {vec4 values;vec4 whiteContrast;vec4 liftSaturation;vec4 gammaSharpen;vec4 gainEnabled;vec4 outputSize;} settings;
vec3 OutputWhiteBalance(vec3 color)
{
    if(all(equal(settings.whiteContrast.xyz,vec3(1))))return color;
    vec3 lms=vec3(dot(color,vec3(.390405,.549941,.00892632)),
        dot(color,vec3(.0708416,.963172,.00135775)),dot(color,vec3(.0231082,.128021,.936245)));
    lms*=settings.whiteContrast.xyz;
    return max(vec3(dot(lms,vec3(2.85847,-1.62879,-.024891)),
        dot(lms,vec3(-.210182,1.1582,.000324281)),dot(lms,vec3(-.041812,-.118169,1.06867))),vec3(0));
}
vec3 OutputColorGrade(vec3 color)
{
    if(settings.gainEnabled.w<.5)return color;
    color=pow(clamp(color*settings.gainEnabled.rgb+settings.liftSaturation.rgb,vec3(0),vec3(1)),
        vec3(1)/max(settings.gammaSharpen.rgb,vec3(.01)));
    color=(color-vec3(.18))*settings.whiteContrast.w+vec3(.18);
    float luminance=dot(color,vec3(.2126,.7152,.0722));
    return clamp(mix(vec3(luminance),color,settings.liftSaturation.w),vec3(0),vec3(1));
}
vec3 OutputMapHdr(vec3 hdr)
{
    if(settings.values.w>.5)return clamp(hdr,vec3(0),vec3(1));
    hdr=min(max(hdr*settings.values.x,vec3(0)),vec3(16777216.0));
    if(settings.gainEnabled.w>.5)hdr=OutputWhiteBalance(hdr);
    hdr=min(hdr,vec3(16777216.0));
    return OutputColorGrade(hdr/(hdr+vec3(1)));
}
void main()
{
    vec4 hdr=textureLod(sampler2D(hdrImage,hdrSampler),uv,0);
    if(settings.values.z>.5)
    {
        // RGBE 내보내기는 노출·색보정·선명도·출력 감마를 모두 우회한다.
        vec3 radiance=clamp(hdr.rgb,vec3(0),vec3(65504));
        float peak=max(radiance.r,max(radiance.g,radiance.b));
        if(peak<=0){color=vec4(0);return;}
        float exponent=clamp(ceil(log2(peak)),-127.0,127.0);
        color=vec4(radiance/exp2(exponent),(exponent+128)/255);return;
    }
    vec3 mapped=OutputMapHdr(hdr.rgb);
    if(settings.gammaSharpen.w>0 && settings.values.w<.5)
    {
        // 색보정 뒤 출력 픽셀 간격으로 선명도를 조절하며 이웃 색 범위를 넘기지 않는다.
        const vec2 offsets[4]=vec2[4](vec2(1,0),vec2(-1,0),vec2(0,1),vec2(0,-1));
        vec3 minimum=mapped,maximum=mapped,blur=vec3(0);float weightSum=0;
        float luminance=dot(mapped,vec3(.2126,.7152,.0722));
        for(int i=0;i<4;++i)
        {
            vec3 neighbor=OutputMapHdr(textureLod(sampler2D(hdrImage,hdrSampler),uv+offsets[i]*settings.outputSize.xy,0).rgb);
            float weight=1/(1+abs(dot(neighbor,vec3(.2126,.7152,.0722))-luminance)*16);
            blur+=neighbor*weight;weightSum+=weight;
            minimum=min(minimum,neighbor);maximum=max(maximum,neighbor);
        }
        mapped=clamp(mapped+settings.gammaSharpen.w*(mapped-blur/max(weightSum,.0001)),minimum,maximum);
    }
    if(settings.values.y>.5)mapped=pow(mapped,vec3(1.0/2.2));
    color=vec4(mapped,hdr.a);
}
