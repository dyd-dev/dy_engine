#include <metal_stdlib>
using namespace metal;
struct ScreenOutput {float4 position [[position]];float2 uv;};
float debugLinearToSrgbChannel(float c) {return c<=0.0031308?c*12.92:1.055*pow(c,1.0/2.4)-0.055;}
float debugSrgbToLinearChannel(float c) {return c<=0.04045?c/12.92:pow((c+0.055)/1.055,2.4);}
float3 debugLinearToSrgb(float3 value)
{
    float3 c=clamp(value,0.0,1.0);
    return float3(debugLinearToSrgbChannel(c.r),debugLinearToSrgbChannel(c.g),debugLinearToSrgbChannel(c.b));
}
float3 debugSrgbToLinear(float3 c)
{
    return float3(debugSrgbToLinearChannel(c.r),debugSrgbToLinearChannel(c.g),debugSrgbToLinearChannel(c.b));
}
float3 debugDecodeNormal(float2 packed)
{
    float2 f=packed*2.0-1.0;
    float3 n=float3(f,1.0-abs(f.x)-abs(f.y));
    if(n.z<0.0)n.xy=(1.0-abs(n.yx))*float2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
    return normalize(n);
}
fragment float4 surfaceDebugFragment(ScreenOutput input [[stage_in]],
    texture2d<float> normalImage [[texture(0)]],texture2d<float> motionImage [[texture(1)]],
    texture2d<float> albedoImage [[texture(2)]],constant float4& operation [[buffer(15)]])
{
    // operation: view, sRGB attachment, camera far, motion pixels/frame range.
    int2 dimensions=int2(motionImage.get_width(),motionImage.get_height());
    uint2 pixel=uint2(clamp(int2(input.uv*float2(dimensions)),int2(0),dimensions-1));
    float4 motion=motionImage.read(pixel);
    float3 displayColor=float3(0);
    if(motion.z>0.0)
    {
        float4 surface=normalImage.read(pixel),material=albedoImage.read(pixel);
        int mode=int(operation.x+0.5);
        if(mode==1)displayColor=debugLinearToSrgb(material.rgb);
        else if(mode==2)displayColor=debugDecodeNormal(surface.xy)*0.5+0.5;
        else if(mode==3)displayColor=float3(clamp(surface.z,0.0,1.0));
        else if(mode==4)displayColor=float3(clamp(material.a,0.0,1.0));
        else if(mode==5)displayColor=float3(clamp(log2(1.0+motion.z)/log2(1.0+operation.z),0.0,1.0));
        else if(mode==6)
        {
            float2 velocityPixels=motion.xy*float2(dimensions);
            displayColor=clamp(float3(float2(0.5)+velocityPixels/(2.0*operation.w),length(velocityPixels)/operation.w),0.0,1.0);
        }
    }
    // Preserve direct display channel values on both UNORM and sRGB targets.
    return float4(operation.y>0.5?debugSrgbToLinear(displayColor):displayColor,1);
}
