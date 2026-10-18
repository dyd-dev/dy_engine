Texture2D<float4> normalImage:register(t0),motionImage:register(t1),albedoImage:register(t2);
// x=SurfaceDebugView, y=sRGB attachment, z=camera far, w=motion pixels/frame range.
cbuffer Pass:register(b15) {float4 operation;};
float linearToSrgbChannel(float c) {return c<=0.0031308?c*12.92:1.055*pow(c,1.0/2.4)-0.055;}
float srgbToLinearChannel(float c) {return c<=0.04045?c/12.92:pow((c+0.055)/1.055,2.4);}
float3 linearToSrgb(float3 value)
{
    float3 c=saturate(value);
    return float3(linearToSrgbChannel(c.r),linearToSrgbChannel(c.g),linearToSrgbChannel(c.b));
}
float3 srgbToLinear(float3 c)
{
    return float3(srgbToLinearChannel(c.r),srgbToLinearChannel(c.g),srgbToLinearChannel(c.b));
}
float3 decodeNormal(float2 packed)
{
    float2 f=packed*2.0-1.0;
    float3 n=float3(f,1.0-abs(f.x)-abs(f.y));
    if(n.z<0.0)n.xy=(1.0-abs(n.yx))*float2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
    return normalize(n);
}
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target0
{
    uint width,height;motionImage.GetDimensions(width,height);
    int2 dimensions=int2(width,height);
    int2 pixel=clamp(int2(uv*float2(dimensions)),int2(0,0),dimensions-1);
    float4 motion=motionImage.Load(int3(pixel,0));
    float3 displayColor=float3(0,0,0);
    if(motion.z>0.0)
    {
        float4 surface=normalImage.Load(int3(pixel,0)),material=albedoImage.Load(int3(pixel,0));
        int mode=int(operation.x+0.5);
        if(mode==1)displayColor=linearToSrgb(material.rgb);
        else if(mode==2)displayColor=decodeNormal(surface.xy)*0.5+0.5;
        else if(mode==3)displayColor=saturate(surface.z).xxx;
        else if(mode==4)displayColor=saturate(material.a).xxx;
        else if(mode==5)displayColor=saturate(log2(1.0+motion.z)/log2(1.0+operation.z)).xxx;
        else if(mode==6)
        {
            float2 velocityPixels=motion.xy*float2(dimensions);
            displayColor=saturate(float3(float2(0.5,0.5)+velocityPixels/(2.0*operation.w),length(velocityPixels)/operation.w));
        }
    }
    // Preserve direct display channel values on both UNORM and sRGB targets.
    return float4(operation.y>0.5?srgbToLinear(displayColor):displayColor,1);
}
