Texture2D<float4> currentImage:register(t0,space0);
Texture2D<float4> motionImage:register(t1,space0);
Texture2D<float4> historyImage:register(t2,space0);
Texture2D<float4> reactiveImage:register(t3,space0);
SamplerState imageSampler:register(s8,space0);
cbuffer Settings:register(b15,space0) {float4 temporal;float4 sizeReject;float4 jitter;column_major float4x4 skyCurrentToPreviousClip;};

float3 ToYCoCg(float3 c)
{
    return float3(dot(c,float3(.25,.5,.25)),.5*c.r-.5*c.b,-.25*c.r+.5*c.g-.25*c.b);
}
float3 ToRGB(float3 c) {return float3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
int2 Pixel(int2 p) {return clamp(p,int2(0,0),int2(sizeReject.xy)-1);}

float4 CubicWeights(float f)
{
    float f2=f*f,f3=f2*f;
    return float4(-.5*f3+f2-.5*f,1.5*f3-2.5*f2+1,-1.5*f3+2*f2+.5*f,.5*f3-.5*f2);
}
float4 MomentWeights(float f)
{
    float g=1-f,f2=f*f,f3=f2*f;
    return float4(g*g*g,3*f3-6*f2+4,-3*f3+3*f2+3*f+1,f3)/6;
}
float3 ReconstructCurrent(float2 coordinate,out float3 lower,out float3 upper)
{
    float2 position=coordinate*sizeReject.xy-.5;
    int2 base=int2(floor(position));float2 f=frac(position);
    float4 wx=CubicWeights(f.x),wy=CubicWeights(f.y),mx=MomentWeights(f.x),my=MomentWeights(f.y);
    float3 reconstructed=0,mean=0,second=0,coreMinimum=1e30,coreMaximum=-1e30;
    for(int y=0;y<4;++y) for(int x=0;x<4;++x)
    {
        float3 c=max(currentImage.Load(int3(Pixel(base+int2(x-1,y-1)),0)).rgb,0);
        reconstructed+=c*(wx[x]*wy[y]);
        float3 encoded=ToYCoCg(c);float weight=mx[x]*my[y];
        mean+=encoded*weight;second+=encoded*encoded*weight;
        if(x>=1 && x<=2 && y>=1 && y<=2)
        {
            coreMinimum=min(coreMinimum,c);coreMaximum=max(coreMaximum,c);
        }
    }
    reconstructed=clamp(reconstructed,coreMinimum,coreMaximum);
    float3 deviation=sqrt(max(second-mean*mean,0)),center=ToYCoCg(reconstructed);
    lower=min(mean-1.5*deviation,center);upper=max(mean+1.5*deviation,center);
    return reconstructed;
}
float3 ReconstructHistory(float2 coordinate)
{
    float2 position=coordinate*sizeReject.xy-.5,base=floor(position),f=frac(position);
    float4 wx=CubicWeights(f.x),wy=CubicWeights(f.y);
    float3 weightX=float3(wx.x,wx.y+wx.z,wx.w),weightY=float3(wy.x,wy.y+wy.z,wy.w);
    float3 px=float3(base.x-.5,base.x+.5+wx.z/weightX.y,base.x+2.5)*temporal.x;
    float3 py=float3(base.y-.5,base.y+.5+wy.z/weightY.y,base.y+2.5)*temporal.y;
    float3 result=0;
    for(int y=0;y<3;++y) for(int x=0;x<3;++x)
        result+=historyImage.SampleLevel(imageSampler,float2(px[x],py[y]),0).rgb*(weightX[x]*weightY[y]);
    return max(result,0);
}

float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    float2 outputUV=position.xy*temporal.xy;
    float2 currentUV=outputUV+jitter.xy;
    int2 pixel=Pixel(int2(currentUV*sizeReject.xy));
    float4 surface=motionImage.Load(int3(pixel,0));
    float4 motion=surface;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
    {
        float4 candidate=motionImage.Load(int3(Pixel(pixel+int2(x,y)),0));
        if(candidate.z>0 && (motion.z<=0 || candidate.z<motion.z)) motion=candidate;
    }
    bool geometry=motion.z>0;
    float3 lower,upper;
    float3 current=ReconstructCurrent(currentUV,lower,upper);
    float3 result=current;
    if(temporal.w>.5 && (!geometry || motion.w>0))
    {
        float2 stableMotion=0,historyUV=-1;
        if(geometry)
        {
            stableMotion=motion.xy-jitter.xy+jitter.zw;
            historyUV=outputUV-stableMotion;
        }
        else
        {
            float4 previousClip=mul(skyCurrentToPreviousClip,float4(outputUV*float2(2,-2)+float2(-1,1),1,1));
            if(previousClip.w>0)
            {
                historyUV=previousClip.xy/previousClip.w*float2(.5,-.5)+.5;
                stableMotion=outputUV-historyUV;
            }
        }
        float2 margin=temporal.xy*.5;
        if(all(historyUV>=margin) && all(historyUV<=1-margin))
        {
            int2 historyPixel=Pixel(int2(historyUV*sizeReject.xy));
            float oldDepth=historyImage.Load(int3(historyPixel,0)).a;
            float tolerance=max(sizeReject.z,sizeReject.w*motion.w);
            bool sameSurface=geometry ? oldDepth>0 && abs(oldDepth-motion.w)<=tolerance : oldDepth==0;
            if(sameSurface)
            {
                float3 rawHistory=ReconstructHistory(historyUV);
                float3 history=ToYCoCg(rawHistory);
                // Evaluate before clipping; this relative HDR difference does not
                // reject static transparency solely because its coverage is one.
                float colorEnergy=dot(current+rawHistory,float3(1,1,1));
                float relativeChange=colorEnergy>0?dot(abs(current-rawHistory),float3(1,1,1))/colorEnergy:0;
                float colorResponse=smoothstep(.05,.25,relativeChange);
                uint reactiveWidth,reactiveHeight;
                reactiveImage.GetDimensions(reactiveWidth,reactiveHeight);
                int2 reactiveSize=int2(reactiveWidth,reactiveHeight);
                int2 reactivePixel=int2(currentUV*reactiveSize);
                float reactive=0;
                for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
                {
                    int2 rp=clamp(reactivePixel+int2(x,y),int2(0,0),reactiveSize-1);
                    reactive=max(reactive,reactiveImage.Load(int3(rp,0)).r);
                }
                history=clamp(history,lower,upper);
                float3 center=ToYCoCg(current);
                float speed=length(stableMotion*sizeReject.xy);
                float weight=temporal.z*lerp(1,.65,saturate(speed/32));
                weight/=1+.5*abs(history.x-center.x)/max(center.x,.15);
                weight*=1-saturate(reactive)*colorResponse;
                result=ToRGB(lerp(center,history,weight));
            }
        }
    }
    return float4(clamp(result,0,64000),geometry ? motion.z : 0);
}
