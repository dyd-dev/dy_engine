#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
layout(set=0,binding=0) uniform texture2D baseImage;
layout(set=0,binding=1) uniform texture2D lowImage;
layout(set=0,binding=8) uniform sampler imageSampler;
layout(push_constant) uniform Settings {vec4 texel;vec4 unused;} settings;
void main()
{
    vec3 blurred=vec3(0);
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
    {
        float weight=float(x==0?2:1)*float(y==0?2:1);
        blurred+=texture(sampler2D(lowImage,imageSampler),uv+vec2(x,y)*settings.texel.xy).rgb*weight;
    }
    vec3 base=texture(sampler2D(baseImage,imageSampler),uv).rgb;
    // Energy stays independent of the number of pyramid levels.
    outputColor=vec4(mix(base,blurred/16,.65),1);
}
