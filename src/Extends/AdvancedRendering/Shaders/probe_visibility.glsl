#ifndef DY_PROBE_VISIBILITY_INCLUDED
#define DY_PROBE_VISIBILITY_INCLUDED
ivec2 DyWrapBilinearX(int low,int high,int width)
{
    if(width>0 && (width&(width-1))==0)
    {
        int mask=width-1;
        return ivec2(low&mask,high&mask);
    }
    return ivec2((low%width+width)%width,(high%width+width)%width);
}
float SpatialProbeVisibility(vec3 position,vec3 probePosition,float valid,float scale,uint layer,
    texture2DArray image,sampler pointSampler,sampler linearSampler,uint enabled)
{
    if(valid<=.5 || scale<=0) return 1;
    vec3 delta=position-probePosition;float distance=length(delta);
    if(distance<=1e-6) return 1;
    vec3 direction=delta/distance;
    vec2 coordinate=vec2(atan(direction.y,direction.x)*.15915494309189535+.5,
        acos(clamp(direction.z,-1,1))*.3183098861837907);
    ivec2 dimensions=textureSize(sampler2DArray(image,pointSampler),0).xy;
    vec2 moments;
    // Integer layers through 2^24 are represented exactly by the sample coordinate.
    if(enabled==1u && layer<=16777216u &&
        !any(isnan(coordinate)) && !any(isinf(coordinate)) &&
        all(greaterThanEqual(coordinate,vec2(0))) && all(lessThanEqual(coordinate,vec2(1))))
        moments=textureLod(sampler2DArray(image,linearSampler),vec3(coordinate,float(layer)),0).rg;
    else
    {
        vec2 pixel=coordinate*vec2(dimensions)-.5,w=fract(pixel);ivec2 a=ivec2(floor(pixel)),b=a+1;
        ivec2 wrappedX=DyWrapBilinearX(a.x,b.x,dimensions.x);
        a.x=wrappedX.x;b.x=wrappedX.y;
        a.y=clamp(a.y,0,dimensions.y-1);b.y=clamp(b.y,0,dimensions.y-1);
        moments=mix(mix(texelFetch(sampler2DArray(image,pointSampler),ivec3(a,int(layer)),0),
            texelFetch(sampler2DArray(image,pointSampler),ivec3(b.x,a.y,int(layer)),0),w.x),
            mix(texelFetch(sampler2DArray(image,pointSampler),ivec3(a.x,b.y,int(layer)),0),
            texelFetch(sampler2DArray(image,pointSampler),ivec3(b,int(layer)),0),w.x),w.y).rg;
    }
    float footprint=moments.x*(3.141592653589793/max(float(dimensions.y),1));
    float excess=max(distance/scale-moments.x-footprint,0);
    float variance=max(moments.y-moments.x*moments.x,footprint*footprint);
    return excess<=0?1:(variance>0?variance/(variance+excess*excess):0);
}
#endif
