#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
layout(set=0,binding=0) uniform texture2D sceneImage;
layout(set=0,binding=1) uniform texture2D bloomImage;
layout(set=0,binding=2) uniform texture2D exposureImage;
layout(set=0,binding=8) uniform sampler imageSampler;
layout(push_constant) uniform Settings {vec4 tone;vec4 unused;vec4 whiteContrast;vec4 liftSaturation;vec4 gammaSharpen;vec4 gainEnabled;vec4 outputSize;} settings;
vec3 AcesFitted(vec3 color)
{
    vec3 v=vec3(dot(color,vec3(.59719,.35458,.04823)),
        dot(color,vec3(.076,.90834,.01566)),dot(color,vec3(.0284,.13383,.83777)));
    vec3 a=v*(v+.0245786)-.000090537;
    vec3 b=v*(.983729*v+.432951)+.238081;
    v=a/b;
    return clamp(vec3(dot(v,vec3(1.60475,-.53108,-.07367)),
        dot(v,vec3(-.10208,1.10813,-.00605)),dot(v,vec3(-.00327,-.07276,1.07602))),vec3(0),vec3(1));
}
float Srgb(float value) {return value<=.0031308?12.92*value:1.055*pow(value,1/2.4)-.055;}
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
vec3 OutputMapped(vec2 coordinate,float exposure)
{
    vec3 hdr=max(textureLod(sampler2D(sceneImage,imageSampler),coordinate,0).rgb,vec3(0));
    if(settings.tone.w>.5)hdr+=max(textureLod(sampler2D(bloomImage,imageSampler),coordinate,0).rgb,vec3(0))*settings.tone.y;
    if(settings.unused.x>.5)return clamp(hdr,vec3(0),vec3(1));
    hdr=min(hdr*exposure,vec3(64000));
    if(settings.gainEnabled.w>.5)hdr=OutputWhiteBalance(hdr);
    return OutputColorGrade(AcesFitted(min(hdr,vec3(64000))));
}
void main()
{
    float exposure=settings.tone.x;
    if(settings.unused.y>.5)exposure*=settings.unused.z/exp2(texelFetch(sampler2D(exposureImage,imageSampler),ivec2(0),0).r);
    vec3 mapped=OutputMapped(uv,exposure);
    if(settings.gammaSharpen.w>0 && settings.unused.x<.5)
    {
        // 출력 픽셀 간격으로 비교하고 큰 휘도 경계의 영향을 줄여 테두리 번짐을 억제한다.
        const vec2 offsets[4]=vec2[4](vec2(1,0),vec2(-1,0),vec2(0,1),vec2(0,-1));
        vec3 minimum=mapped,maximum=mapped,blur=vec3(0);float weightSum=0;
        float luminance=dot(mapped,vec3(.2126,.7152,.0722));
        for(int i=0;i<4;++i)
        {
            vec3 neighbor=OutputMapped(uv+offsets[i]*settings.outputSize.xy,exposure);
            float weight=1/(1+abs(dot(neighbor,vec3(.2126,.7152,.0722))-luminance)*16);
            blur+=neighbor*weight;weightSum+=weight;
            minimum=min(minimum,neighbor);maximum=max(maximum,neighbor);
        }
        mapped=clamp(mapped+settings.gammaSharpen.w*(mapped-blur/max(weightSum,.0001)),minimum,maximum);
    }
    if(settings.tone.z>.5)mapped=vec3(Srgb(mapped.r),Srgb(mapped.g),Srgb(mapped.b));
    outputColor=vec4(mapped,1);
}
