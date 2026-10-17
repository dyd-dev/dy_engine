#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform texture2D normalImage;
layout(set=0,binding=1) uniform texture2D motionImage;
layout(set=0,binding=2) uniform texture2D albedoImage;
layout(set=0,binding=7) uniform sampler nearestSampler;
// x=SurfaceDebugView, y=sRGB attachment, z=camera far, w=motion pixels/frame range.
layout(push_constant) uniform Pass {vec4 operation;};

vec3 linearToSrgb(vec3 value)
{
    vec3 c=clamp(value,vec3(0),vec3(1));
    return mix(1.055*pow(c,vec3(1.0/2.4))-0.055,c*12.92,lessThanEqual(c,vec3(0.0031308)));
}
vec3 srgbToLinear(vec3 c)
{
    return mix(pow((c+0.055)/1.055,vec3(2.4)),c/12.92,lessThanEqual(c,vec3(0.04045)));
}
vec3 decodeNormal(vec2 packed)
{
    vec2 f=packed*2.0-1.0;
    vec3 n=vec3(f,1.0-abs(f.x)-abs(f.y));
    if(n.z<0.0)n.xy=(1.0-abs(n.yx))*vec2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
    return normalize(n);
}
void main()
{
    ivec2 dimensions=textureSize(sampler2D(motionImage,nearestSampler),0);
    ivec2 pixel=clamp(ivec2(uv*vec2(dimensions)),ivec2(0),dimensions-1);
    vec4 motion=texelFetch(sampler2D(motionImage,nearestSampler),pixel,0);
    vec3 displayColor=vec3(0);
    if(motion.z>0.0)
    {
        vec4 surface=texelFetch(sampler2D(normalImage,nearestSampler),pixel,0),
            material=texelFetch(sampler2D(albedoImage,nearestSampler),pixel,0);
        int mode=int(operation.x+0.5);
        if(mode==1)displayColor=linearToSrgb(material.rgb);
        else if(mode==2)displayColor=decodeNormal(surface.xy)*0.5+0.5;
        else if(mode==3)displayColor=vec3(clamp(surface.z,0.0,1.0));
        else if(mode==4)displayColor=vec3(clamp(material.a,0.0,1.0));
        else if(mode==5)displayColor=vec3(clamp(log2(1.0+motion.z)/log2(1.0+operation.z),0.0,1.0));
        else if(mode==6)
        {
            vec2 velocityPixels=motion.xy*vec2(dimensions);
            displayColor=clamp(vec3(vec2(0.5)+velocityPixels/(2.0*operation.w),length(velocityPixels)/operation.w),0.0,1.0);
        }
    }
    // Debug channels are display values. Undo their encoding only when the
    // render-target hardware will encode them again on the attachment write.
    outColor=vec4(operation.y>0.5?srgbToLinear(displayColor):displayColor,1);
}
