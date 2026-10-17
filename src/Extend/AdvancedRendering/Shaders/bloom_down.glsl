#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
layout(set=0,binding=0) uniform texture2D image;
layout(set=0,binding=8) uniform sampler imageSampler;
layout(push_constant) uniform Settings {vec4 texelThreshold;vec4 mode;} settings;
const vec2 offsets[13]=vec2[13](
    vec2(-2,-2),vec2(0,-2),vec2(2,-2),vec2(-2,0),vec2(0,0),vec2(2,0),
    vec2(-2,2),vec2(0,2),vec2(2,2),vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(1,1));
const float weights[13]=float[13](.03125,.0625,.03125,.0625,.125,.0625,.03125,.0625,.03125,.125,.125,.125,.125);
void main()
{
    vec3 filtered=vec3(0);
    float total=0;
    for(int i=0;i<13;++i)
    {
        vec3 sampleColor=clamp(texture(sampler2D(image,imageSampler),uv+offsets[i]*settings.texelThreshold.xy).rgb,vec3(0),vec3(64000));
        // The first stage reduces isolated HDR fireflies before extracting glow.
        float weight=weights[i];
        if(settings.mode.x>.5) weight/=1+dot(sampleColor,vec3(.2126,.7152,.0722));
        filtered+=sampleColor*weight;total+=weight;
    }
    filtered/=max(total,1e-6);
    if(settings.mode.x>.5)
    {
        float brightness=max(max(filtered.r,filtered.g),filtered.b);
        float threshold=settings.texelThreshold.z,knee=settings.texelThreshold.w;
        float soft=clamp(brightness-threshold+knee,0,2*knee);
        soft=soft*soft/max(4*knee,1e-5);
        filtered*=max(soft,brightness-threshold)/max(brightness,1e-5);
    }
    outputColor=vec4(filtered,1);
}
