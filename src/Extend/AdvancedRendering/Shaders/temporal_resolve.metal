#include <metal_stdlib>
using namespace metal;
struct Input {float4 position [[position]];float2 uv [[user(locn0)]];};
struct Settings {float4 temporal;float4 sizeReject;float4 jitter;float4x4 skyCurrentToPreviousClip;};
float3 ToYCoCg(float3 c)
{
    return float3(dot(c,float3(.25,.5,.25)),.5*c.r-.5*c.b,-.25*c.r+.5*c.g-.25*c.b);
}
float3 ToRGB(float3 c) {return float3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
uint2 Pixel(int2 p,constant Settings& settings)
{
    return uint2(clamp(p,int2(0),int2(settings.sizeReject.xy)-1));
}
float4 CubicWeights(float f)
{
    float f2=f*f,f3=f2*f;
    return float4(-.5f*f3+f2-.5f*f,1.5f*f3-2.5f*f2+1,-1.5f*f3+2*f2+.5f*f,.5f*f3-.5f*f2);
}
float4 MomentWeights(float f)
{
    float g=1-f,f2=f*f,f3=f2*f;
    return float4(g*g*g,3*f3-6*f2+4,-3*f3+3*f2+3*f+1,f3)/6;
}
float3 ReconstructCurrent(texture2d<float> image,float2 coordinate,constant Settings& settings,
    thread float3& lower,thread float3& upper)
{
    float2 position=coordinate*settings.sizeReject.xy-.5f;
    int2 base=int2(floor(position));float2 f=fract(position);
    float4 wx=CubicWeights(f.x),wy=CubicWeights(f.y),mx=MomentWeights(f.x),my=MomentWeights(f.y);
    float3 reconstructed=0,mean=0,second=0,coreMinimum=1e30f,coreMaximum=-1e30f;
    for(int y=0;y<4;++y) for(int x=0;x<4;++x)
    {
        float3 c=max(image.read(Pixel(base+int2(x-1,y-1),settings)).rgb,0.f);
        reconstructed+=c*(wx[x]*wy[y]);
        float3 encoded=ToYCoCg(c);float weight=mx[x]*my[y];
        mean+=encoded*weight;second+=encoded*encoded*weight;
        if(x>=1 && x<=2 && y>=1 && y<=2)
        {
            coreMinimum=min(coreMinimum,c);coreMaximum=max(coreMaximum,c);
        }
    }
    reconstructed=clamp(reconstructed,coreMinimum,coreMaximum);
    float3 deviation=sqrt(max(second-mean*mean,0.f)),center=ToYCoCg(reconstructed);
    lower=min(mean-1.5f*deviation,center);upper=max(mean+1.5f*deviation,center);
    return reconstructed;
}
float3 ReconstructHistory(texture2d<float> image,sampler imageSampler,float2 coordinate,constant Settings& settings)
{
    float2 position=coordinate*settings.sizeReject.xy-.5f,base=floor(position),f=fract(position);
    float4 wx=CubicWeights(f.x),wy=CubicWeights(f.y);
    float3 weightX=float3(wx.x,wx.y+wx.z,wx.w),weightY=float3(wy.x,wy.y+wy.z,wy.w);
    float3 px=float3(base.x-.5f,base.x+.5f+wx.z/weightX.y,base.x+2.5f)*settings.temporal.x;
    float3 py=float3(base.y-.5f,base.y+.5f+wy.z/weightY.y,base.y+2.5f)*settings.temporal.y;
    float3 result=0;
    for(int y=0;y<3;++y) for(int x=0;x<3;++x)
        result+=image.sample(imageSampler,float2(px[x],py[y]),level(0)).rgb*(weightX[x]*weightY[y]);
    return max(result,0.f);
}
fragment float4 fragmentMain(Input input [[stage_in]],
    texture2d<float> currentImage [[texture(0)]],texture2d<float> motionImage [[texture(1)]],
    texture2d<float> historyImage [[texture(2)]],texture2d<float> reactiveImage [[texture(3)]],
    sampler imageSampler [[sampler(8)]],
    constant Settings& settings [[buffer(15)]])
{
    float2 outputUV=input.position.xy*settings.temporal.xy;
    float2 currentUV=outputUV+settings.jitter.xy;
    int2 pixel=int2(currentUV*settings.sizeReject.xy);
    float4 surface=motionImage.read(Pixel(pixel,settings));
    float4 motion=surface;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
    {
        float4 candidate=motionImage.read(Pixel(pixel+int2(x,y),settings));
        if(candidate.z>0 && (motion.z<=0 || candidate.z<motion.z)) motion=candidate;
    }
    bool geometry=motion.z>0;
    float3 lower,upper;
    float3 current=ReconstructCurrent(currentImage,currentUV,settings,lower,upper);
    float3 result=current;
    if(settings.temporal.w>.5 && (!geometry || motion.w>0))
    {
        float2 stableMotion=0,historyUV=-1;
        if(geometry)
        {
            stableMotion=motion.xy-settings.jitter.xy+settings.jitter.zw;
            historyUV=outputUV-stableMotion;
        }
        else
        {
            float4 previousClip=settings.skyCurrentToPreviousClip*float4(outputUV*float2(2,-2)+float2(-1,1),1,1);
            if(previousClip.w>0)
            {
                historyUV=previousClip.xy/previousClip.w*float2(.5,-.5)+.5;
                stableMotion=outputUV-historyUV;
            }
        }
        float2 margin=settings.temporal.xy*.5;
        if(all(historyUV>=margin) && all(historyUV<=1.f-margin))
        {
            uint2 historyPixel=Pixel(int2(historyUV*settings.sizeReject.xy),settings);
            float oldDepth=historyImage.read(historyPixel).a;
            float tolerance=max(settings.sizeReject.z,settings.sizeReject.w*motion.w);
            bool sameSurface=geometry ? oldDepth>0 && abs(oldDepth-motion.w)<=tolerance : oldDepth==0;
            if(sameSurface)
            {
                float3 rawHistory=ReconstructHistory(historyImage,imageSampler,historyUV,settings);
                float3 history=ToYCoCg(rawHistory);
                // Use unclipped relative HDR change, retaining stable glass history.
                float colorEnergy=dot(current+rawHistory,float3(1));
                float relativeChange=colorEnergy>0?dot(abs(current-rawHistory),float3(1))/colorEnergy:0;
                float colorResponse=smoothstep(.05f,.25f,relativeChange);
                int2 reactiveSize=int2(reactiveImage.get_width(),reactiveImage.get_height());
                int2 reactivePixel=int2(currentUV*float2(reactiveSize));
                float reactive=0;
                for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
                {
                    uint2 rp=uint2(clamp(reactivePixel+int2(x,y),int2(0),reactiveSize-1));
                    reactive=max(reactive,reactiveImage.read(rp).r);
                }
                history=clamp(history,lower,upper);
                float3 center=ToYCoCg(current);
                float speed=length(stableMotion*settings.sizeReject.xy);
                float weight=settings.temporal.z*mix(1.f,.65f,clamp(speed/32.f,0.f,1.f));
                weight/=1+.5*abs(history.x-center.x)/max(center.x,.15f);
                weight*=1.f-clamp(reactive,0.f,1.f)*colorResponse;
                result=ToRGB(mix(center,history,weight));
            }
        }
    }
    return float4(clamp(result,0.f,64000.f),geometry ? motion.z : 0);
}
