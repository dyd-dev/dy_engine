#version 450
layout(location=0) out vec4 outputColor;
layout(set=0,binding=0) uniform texture2D sourceImage;
layout(set=0,binding=1) uniform texture2D previousImage;
layout(set=0,binding=8) uniform sampler imageSampler;
layout(push_constant) uniform Settings {vec4 operation;} settings;
const float minimumLogLuminance=-13.28771238; // log2(0.0001)
void main()
{
    if(settings.operation.x>1.5)
    {
        float target=texelFetch(sampler2D(sourceImage,imageSampler),ivec2(0),0).r;
        if(isnan(target) || isinf(target)) target=minimumLogLuminance;
        float adapted=target;
        if(settings.operation.y>.5)
        {
            float previous=texelFetch(sampler2D(previousImage,imageSampler),ivec2(0),0).r;
            if(!isnan(previous) && !isinf(previous)) adapted=mix(previous,target,settings.operation.z);
        }
        outputColor=vec4(adapted,target,0,1);
        return;
    }
    ivec2 size=textureSize(sampler2D(sourceImage,imageSampler),0);
    ivec2 origin=ivec2(gl_FragCoord.xy)*4;
    float weightedLog=0,weight=0;
    for(int y=0;y<4;++y) for(int x=0;x<4;++x)
    {
        ivec2 pixel=origin+ivec2(x,y);
        if(any(greaterThanEqual(pixel,size))) continue;
        vec4 value=texelFetch(sampler2D(sourceImage,imageSampler),pixel,0);
        if(settings.operation.x<.5)
        {
            if(any(isnan(value.rgb)) || any(isinf(value.rgb))) continue;
            float luminance=dot(clamp(value.rgb,vec3(0),vec3(65504)),vec3(.2126,.7152,.0722));
            weightedLog+=log2(max(luminance,.0001));weight+=1;
        }
        else
        {
            weightedLog+=value.r*value.g;weight+=value.g;
        }
    }
    // Coverage is relative to the level's nominal footprint. This preserves
    // edge weights without storing pixel counts that overflow half precision.
    outputColor=vec4(weight>0?weightedLog/weight:minimumLogLuminance,weight/16,0,1);
}
