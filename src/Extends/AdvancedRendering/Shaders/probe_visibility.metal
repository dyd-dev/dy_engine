#ifndef DY_PROBE_VISIBILITY_INCLUDED
#define DY_PROBE_VISIBILITY_INCLUDED
inline int2 DyWrapBilinearX(int low,int high,int width)
{
    if(width>0 && (width&(width-1))==0)
    {
        const int mask=width-1;
        return int2(low&mask,high&mask);
    }
    return int2((low%width+width)%width,(high%width+width)%width);
}
inline float SpatialProbeVisibility(float3 position,float3 probePosition,float valid,float scale,uint layer,
    texture2d_array<float> image,sampler pointSampler,sampler linearSampler,uint enabled)
{
    if(valid<=.5f || scale<=0) return 1;
    float3 delta=position-probePosition;float distance=length(delta);
    if(distance<=1e-6f) return 1;
    float3 direction=delta/distance;
    float2 coordinate=float2(atan2(direction.y,direction.x)*.15915494309189535f+.5f,
        acos(clamp(direction.z,-1.0f,1.0f))*.3183098861837907f);
    int2 dimensions=int2(image.get_width(),image.get_height());
    float2 moments;
    // Keep the layer guard aligned with the floating-coordinate backends.
    if(enabled==1u && layer<=16777216u && all(isfinite(coordinate)) &&
        all(coordinate>=0) && all(coordinate<=1))
        moments=image.sample(linearSampler,coordinate,layer,level(0)).rg;
    else
    {
        float2 pixel=coordinate*float2(dimensions)-.5f,w=fract(pixel);int2 a=int2(floor(pixel)),b=a+1;
        const int2 wrappedX=DyWrapBilinearX(a.x,b.x,dimensions.x);
        a.x=wrappedX.x;b.x=wrappedX.y;
        a.y=clamp(a.y,0,dimensions.y-1);b.y=clamp(b.y,0,dimensions.y-1);
        moments=mix(mix(image.read(uint2(a),layer,0),image.read(uint2(b.x,a.y),layer,0),w.x),
            mix(image.read(uint2(a.x,b.y),layer,0),image.read(uint2(b),layer,0),w.x),w.y).rg;
    }
    float footprint=moments.x*(3.141592653589793f/max(float(image.get_height()),1.0f));
    float excess=max(distance/scale-moments.x-footprint,0.0f);
    float variance=max(moments.y-moments.x*moments.x,footprint*footprint);
    return excess<=0?1:(variance>0?variance/(variance+excess*excess):0);
}
#endif
