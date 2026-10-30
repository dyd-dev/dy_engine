#include <metal_stdlib>
using namespace metal;
struct Input {float4 position [[position]];float2 uv [[user(locn0)]];};
struct Settings {float4 tone;float4 unused;float4 whiteContrast;float4 liftSaturation;float4 gammaSharpen;float4 gainEnabled;float4 outputSize;};
float3 AcesFitted(float3 color)
{
    float3 v=float3(dot(color,float3(.59719,.35458,.04823)),
        dot(color,float3(.076,.90834,.01566)),dot(color,float3(.0284,.13383,.83777)));
    float3 a=v*(v+.0245786)-.000090537;
    float3 b=v*(.983729*v+.432951)+.238081;
    v=a/b;
    return clamp(float3(dot(v,float3(1.60475,-.53108,-.07367)),
        dot(v,float3(-.10208,1.10813,-.00605)),dot(v,float3(-.00327,-.07276,1.07602))),0.f,1.f);
}
float Srgb(float value) {return value<=.0031308?12.92*value:1.055*pow(value,1.f/2.4f)-.055;}
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
float3 OutputMapped(float2 coordinate,float exposure,texture2d<float> sceneImage,texture2d<float> bloomImage,sampler imageSampler,constant Settings& settings)
{
    float3 hdr=max(sceneImage.sample(imageSampler,coordinate,level(0)).rgb,float3(0));
    if(settings.tone.w>.5)hdr+=max(bloomImage.sample(imageSampler,coordinate,level(0)).rgb,float3(0))*settings.tone.y;
    if(settings.unused.x>.5)return clamp(hdr,float3(0),float3(1));
    hdr=min(hdr*exposure,float3(64000));
    if(settings.gainEnabled.w>.5)hdr=OutputWhiteBalance(hdr,settings);
    return OutputColorGrade(AcesFitted(min(hdr,float3(64000))),settings);
}
fragment float4 fragmentMain(Input input [[stage_in]],texture2d<float> sceneImage [[texture(0)]],
    texture2d<float> bloomImage [[texture(1)]],texture2d<float> exposureImage [[texture(2)]],sampler imageSampler [[sampler(8)]],
    constant Settings& settings [[buffer(15)]])
{
    float exposure=settings.tone.x;
    if(settings.unused.y>.5)exposure*=settings.unused.z/exp2(exposureImage.read(uint2(0)).r);
    float3 mapped=OutputMapped(input.uv,exposure,sceneImage,bloomImage,imageSampler,settings);
    if(settings.gammaSharpen.w>0 && settings.unused.x<.5)
    {
        // 출력 픽셀 간격으로 비교하고 큰 휘도 경계의 영향을 줄여 테두리 번짐을 억제한다.
        const float2 offsets[4]={float2(1,0),float2(-1,0),float2(0,1),float2(0,-1)};
        float3 minimum=mapped,maximum=mapped,blur=float3(0);float weightSum=0;
        float luminance=dot(mapped,float3(.2126,.7152,.0722));
        for(int i=0;i<4;++i)
        {
            float3 neighbor=OutputMapped(input.uv+offsets[i]*settings.outputSize.xy,exposure,sceneImage,bloomImage,imageSampler,settings);
            float weight=1/(1+abs(dot(neighbor,float3(.2126,.7152,.0722))-luminance)*16);
            blur+=neighbor*weight;weightSum+=weight;
            minimum=min(minimum,neighbor);maximum=max(maximum,neighbor);
        }
        mapped=clamp(mapped+settings.gammaSharpen.w*(mapped-blur/max(weightSum,.0001)),minimum,maximum);
    }
    if(settings.tone.z>.5)mapped=float3(Srgb(mapped.r),Srgb(mapped.g),Srgb(mapped.b));
    return float4(mapped,1);
}
