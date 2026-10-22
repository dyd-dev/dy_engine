#ifndef DY_PROBE_VISIBILITY_INCLUDED
#define DY_PROBE_VISIBILITY_INCLUDED
int2 DyWrapBilinearX(int low,int high,int width)
{
    if(width>0 && (width&(width-1))==0)
    {
        int mask=width-1;
        return int2(low&mask,high&mask);
    }
    return int2((low%width+width)%width,(high%width+width)%width);
}
float SpatialProbeVisibility(float3 position,float3 probePosition,float valid,float scale,uint layer,
    Texture2DArray<float4> image,SamplerState pointSampler,SamplerState linearSampler,uint enabled)
{
    if(valid<=.5 || scale<=0) return 1;
    float3 delta=position-probePosition;float distance=length(delta);
    if(distance<=1e-6) return 1;
    float3 direction=delta/distance;
    float2 coordinate=float2(atan2(direction.y,direction.x)*.15915494309189535+.5,
        acos(clamp(direction.z,-1,1))*.3183098861837907);
    uint width,height,layers,levels;image.GetDimensions(0,width,height,layers,levels);
    int2 dimensions=int2(width,height);float2 moments;
    // Integer layers through 2^24 are represented exactly by the sample coordinate.
    if(enabled==1u && layer<=16777216u && all(isfinite(coordinate)) &&
        all(coordinate>=0) && all(coordinate<=1))
        moments=image.SampleLevel(linearSampler,float3(coordinate,float(layer)),0).rg;
    else
    {
        float2 pixel=coordinate*float2(dimensions)-.5,w=frac(pixel);
        int2 a=int2(floor(pixel)),b=a+1;
        int2 wrappedX=DyWrapBilinearX(a.x,b.x,dimensions.x);
        a.x=wrappedX.x;b.x=wrappedX.y;
        a.y=clamp(a.y,0,dimensions.y-1);b.y=clamp(b.y,0,dimensions.y-1);
        moments=lerp(lerp(image.Load(int4(a,layer,0)),image.Load(int4(b.x,a.y,layer,0)),w.x),
            lerp(image.Load(int4(a.x,b.y,layer,0)),image.Load(int4(b,layer,0)),w.x),w.y).rg;
    }
    float footprint=moments.x*(3.141592653589793/max(float(height),1));
    float excess=max(distance/scale-moments.x-footprint,0);
    float variance=max(moments.y-moments.x*moments.x,footprint*footprint);
    return excess<=0?1:(variance>0?variance/(variance+excess*excess):0);
}
#endif
