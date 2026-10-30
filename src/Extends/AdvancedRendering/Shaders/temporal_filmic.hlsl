Texture2D<float4> sceneImage:register(t0,space0);
Texture2D<float4> bloomImage:register(t1,space0);
Texture2D<float4> exposureImage:register(t2,space0);
SamplerState imageSampler:register(s8,space0);
cbuffer Settings:register(b15,space0) {float4 tone;float4 unused;float4 whiteContrast;float4 liftSaturation;float4 gammaSharpen;float4 gainEnabled;float4 outputSize;};
float3 AcesFitted(float3 color)
{
    float3 v=float3(dot(color,float3(.59719,.35458,.04823)),
        dot(color,float3(.076,.90834,.01566)),dot(color,float3(.0284,.13383,.83777)));
    float3 a=v*(v+.0245786)-.000090537;
    float3 b=v*(.983729*v+.432951)+.238081;
    v=a/b;
    return saturate(float3(dot(v,float3(1.60475,-.53108,-.07367)),
        dot(v,float3(-.10208,1.10813,-.00605)),dot(v,float3(-.00327,-.07276,1.07602))));
}
float Srgb(float value) {return value<=.0031308?12.92*value:1.055*pow(value,1/2.4)-.055;}
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
    color=clamp(color*gainEnabled.rgb+liftSaturation.rgb,float3(0,0,0),float3(1,1,1));
    if(any(gammaSharpen.rgb!=float3(1,1,1)))
        color=pow(color,float3(1,1,1)/max(gammaSharpen.rgb,float3(.01,.01,.01)));
    color=(color-float3(.18,.18,.18))*whiteContrast.w+float3(.18,.18,.18);
    float luminance=dot(color,float3(.2126,.7152,.0722));
    return clamp(lerp(float3(luminance,luminance,luminance),color,liftSaturation.w),float3(0,0,0),float3(1,1,1));
}
float3 OutputMapped(float2 coordinate,float exposure)
{
    float3 hdr=max(sceneImage.SampleLevel(imageSampler,coordinate,0).rgb,float3(0,0,0));
    if(tone.w>.5)hdr+=max(bloomImage.SampleLevel(imageSampler,coordinate,0).rgb,float3(0,0,0))*tone.y;
    if(unused.x>.5)return clamp(hdr,float3(0,0,0),float3(1,1,1));
    hdr=min(hdr*exposure,float3(64000,64000,64000));
    if(gainEnabled.w>.5)hdr=OutputWhiteBalance(hdr);
    return OutputColorGrade(AcesFitted(min(hdr,float3(64000,64000,64000))));
}
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    float exposure=tone.x;
    if(unused.y>.5)exposure*=unused.z/exp2(exposureImage.Load(int3(0,0,0)).r);
    float3 mapped=OutputMapped(uv,exposure);
    if(gammaSharpen.w>0 && unused.x<.5)
    {
        // 출력 픽셀 간격으로 비교하고 큰 휘도 경계의 영향을 줄여 테두리 번짐을 억제한다.
        const float2 offsets[4]={float2(1,0),float2(-1,0),float2(0,1),float2(0,-1)};
        float3 minimum=mapped,maximum=mapped,blur=float3(0,0,0);float weightSum=0;
        float luminance=dot(mapped,float3(.2126,.7152,.0722));
        for(int i=0;i<4;++i)
        {
            float3 neighbor=OutputMapped(uv+offsets[i]*outputSize.xy,exposure);
            float weight=1/(1+abs(dot(neighbor,float3(.2126,.7152,.0722))-luminance)*16);
            blur+=neighbor*weight;weightSum+=weight;
            minimum=min(minimum,neighbor);maximum=max(maximum,neighbor);
        }
        mapped=clamp(mapped+gammaSharpen.w*(mapped-blur/max(weightSum,.0001)),minimum,maximum);
    }
    if(tone.z>.5)mapped=float3(Srgb(mapped.r),Srgb(mapped.g),Srgb(mapped.b));
    return float4(mapped,1);
}
