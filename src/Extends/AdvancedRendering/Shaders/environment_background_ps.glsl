#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
layout(set=0,binding=0) uniform texture2D depthTexture;
layout(set=0,binding=6) uniform texture2D radianceTexture;
layout(set=0,binding=7) uniform sampler environmentSampler;
layout(set=0,binding=9) uniform FrameConstants
{
    mat4 inverseViewProjection;
    vec4 cameraPosition;
    vec4 sh[9];
    vec4 settings;
    vec4 screen;
    vec4 probePosition;
    vec4 probeBoxMin;
    vec4 probeBoxMax;
    vec4 specularColor;
    mat4 backgroundRayToWorld;
} frame;
vec3 SampleRadiance(vec2 coordinate)
{
    ivec2 size=textureSize(sampler2D(radianceTexture,environmentSampler),0);
    vec2 pixel=coordinate*vec2(size)-0.5,weight=fract(pixel);
    ivec2 low=ivec2(floor(pixel)),high=low+1;
    low.x=(low.x%size.x+size.x)%size.x;high.x=(high.x%size.x+size.x)%size.x;
    low.y=clamp(low.y,0,size.y-1);high.y=clamp(high.y,0,size.y-1);
    return mix(mix(texelFetch(sampler2D(radianceTexture,environmentSampler),low,0),texelFetch(sampler2D(radianceTexture,environmentSampler),ivec2(high.x,low.y),0),weight.x),
        mix(texelFetch(sampler2D(radianceTexture,environmentSampler),ivec2(low.x,high.y),0),texelFetch(sampler2D(radianceTexture,environmentSampler),high,0),weight.x),weight.y).rgb;
}
void main()
{
    if(texelFetch(sampler2D(depthTexture,environmentSampler),ivec2(gl_FragCoord.xy),0).r<1.0) discard;
    vec2 coordinate=gl_FragCoord.xy*frame.screen.xy;
    vec2 clip=coordinate*vec2(2,-2)+vec2(-1,1);
    vec4 nearPoint=frame.backgroundRayToWorld*vec4(clip,0,1),farPoint=frame.backgroundRayToWorld*vec4(clip,1,1);
    vec3 direction=normalize(farPoint.xyz*nearPoint.w-nearPoint.xyz*farPoint.w);
    float c=cos(frame.settings.z),s=sin(frame.settings.z);
    direction.xy=mat2(c,-s,s,c)*direction.xy;
    vec2 skyUV=vec2(atan(direction.y,direction.x)*0.15915494309189535+0.5,acos(clamp(direction.z,-1.0,1.0))*0.3183098861837907);
    outputColor=vec4(clamp(SampleRadiance(skyUV)*frame.settings.y,vec3(0),vec3(65504)),1);
}
