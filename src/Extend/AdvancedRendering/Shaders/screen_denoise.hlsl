#ifndef SCREEN_DENOISE_SPATIAL
#define SCREEN_DENOISE_SPATIAL 0
#endif
Texture2D<float4> normalImage:register(t0),motionImage:register(t1),sourceImage:register(t2);
Texture2D<float4> historyImage:register(t3),momentImage:register(t4),guideImage:register(t5);
SamplerState pointSampler:register(s7);
cbuffer Frame:register(b12) {column_major float4x4 view;column_major float4x4 projection;column_major float4x4 inverseProjection;column_major float4x4 viewProjection;float4 size;float4 ao;float4 lighting;float4 options;};
cbuffer Pass:register(b15) {float4 operation;};
float4 Load(Texture2D<float4> image,int2 p)
{
    uint w,h;image.GetDimensions(w,h);return image.Load(int3(clamp(p,int2(0,0),int2(w,h)-1),0));
}
float3 Normal(float2 packed)
{
    float2 f=packed*2-1;float3 n=float3(f,1-abs(f.x)-abs(f.y));
    if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);
    return normalize(n);
}
float4 FiniteColor(float4 value)
{
    if(!all(isfinite(value)))return 0;
    return float4(clamp(value.rgb,0,65504),saturate(value.a));
}
float Luminance(float3 c) {return dot(c,float3(.2126,.7152,.0722));}
float LogLuminance(float3 c) {return log2(1+max(Luminance(c),0));}
float3 SetLogLuminance(float3 c,float value)
{
    float l=Luminance(c);return l>1e-6?c*((exp2(max(value,0))-1)/l):float3(0,0,0);
}
int2 Representative(int2 halfPixel)
{
    int2 dimensions=int2(size.xy),base=halfPixel*2,selected=min(base,dimensions-1);float nearest=1e30;
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
    {
        int2 p=min(base+int2(x,y),dimensions-1);float depth=Load(motionImage,p).z;
        if(depth>0 && depth<nearest) {nearest=depth;selected=p;}
    }
    return selected;
}
struct Input {float4 position:SV_Position;float2 uv:TEXCOORD0;};
#if SCREEN_DENOISE_SPATIAL
float4 main(Input input):SV_Target
{
    int2 pixel=int2(input.position.xy);float4 guide=Load(guideImage,pixel),center=FiniteColor(Load(sourceImage,pixel));
    if(guide.r<=0)return 0;
    float4 moments=Load(momentImage,pixel);float centerLog=LogLuminance(center.rgb);
    float sigma=sqrt(max(moments.g-moments.r*moments.r,0));
    float3 n=Normal(guide.gb);float4 sum=0;float total=0;int step=int(operation.w+.5);
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
    {
        int2 q=pixel+int2(x,y)*step;float4 g=Load(guideImage,q);if(g.r<=0)continue;
        float4 color=FiniteColor(Load(sourceImage,q));float weight=float((x==0?2:1)*(y==0?2:1));
        weight*=exp(-abs(g.r-guide.r)/max(.01,guide.r*.01*float(step)));
        weight*=pow(max(dot(n,Normal(g.gb)),0),32)*exp(-abs(g.a-guide.a)*16);
        weight*=exp(-abs(LogLuminance(color.rgb)-centerLog)/max(4*sigma,.001));
        if(operation.y>.5)weight*=exp(-abs(color.a-center.a)*4);
        sum+=color*weight;total+=weight;
    }
    return FiniteColor(total>1e-5?sum/total:center);
}
#else
struct Output {float4 color:SV_Target0;float4 moments:SV_Target1;float4 guide:SV_Target2;};
Output main(Input input)
{
    Output output;int2 pixel=int2(input.position.xy),fullPixel=Representative(pixel);
    float4 motion=Load(motionImage,fullPixel),surface=Load(normalImage,fullPixel),current=FiniteColor(Load(sourceImage,pixel));
    output.guide=float4(max(motion.z,0),surface.xy,saturate(surface.z));
    if(motion.z<=0) {output.color=0;output.moments=0;output.guide=0;return output;}
    float mean=0,second=0;
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
    {
        float l=LogLuminance(FiniteColor(Load(sourceImage,pixel+int2(x,y))).rgb);mean+=l/9;second+=l*l/9;
    }
    float variance=max(second-mean*mean,0),currentLog=LogLuminance(current.rgb);
    float4 oldColor=0,oldMoments=0;float accepted=0;
    float2 currentUV=(float2(fullPixel)+.5)*size.zw,previousUV=currentUV-motion.xy;
    uint w,h;sourceImage.GetDimensions(w,h);int2 dimensions=int2(w,h);
    if(operation.z>.5 && motion.w>0 && all(previousUV>=0) && all(previousUV<=1))
    {
        float2 previousPixel=previousUV*float2(dimensions)-.5;int2 base=int2(floor(previousPixel));float2 f=frac(previousPixel);
        float3 n=Normal(surface.xy);
        for(int y=0;y<2;++y)for(int x=0;x<2;++x)
        {
            int2 q=base+int2(x,y);if(any(q<0) || any(q>=dimensions))continue;
            float4 guide=Load(guideImage,q);
            if(guide.r<=0 || abs(guide.r-motion.w)>max(.01,motion.w*.01) ||
                dot(n,Normal(guide.gb))<.9 || abs(guide.a-surface.z)>.1)continue;
            float weight=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);
            oldColor+=FiniteColor(Load(historyImage,q))*weight;oldMoments+=Load(momentImage,q)*weight;accepted+=weight;
        }
    }
    float historyLength=1;
    if(accepted>1e-4)
    {
        oldColor/=accepted;oldMoments/=accepted;float oldVariance=max(oldMoments.g-oldMoments.r*oldMoments.r,0);
        float high=max(mean+2*sqrt(variance),oldMoments.r+3*sqrt(oldVariance));
        if(currentLog>high) {current.rgb=SetLogLuminance(current.rgb,high);currentLog=high;}
        float oldLog=LogLuminance(oldColor.rgb),low=min(currentLog,mean-2*sqrt(variance));high=max(currentLog,mean+2*sqrt(variance));
        oldColor.rgb=SetLogLuminance(oldColor.rgb,clamp(oldLog,max(low,0),high));
        float change=abs(currentLog-oldMoments.r)/max(1,sqrt(variance+oldVariance));
        if(operation.y>.5)change=max(change,abs(current.a-oldColor.a));
        float weight=min(oldMoments.b/(oldMoments.b+1),.95)*(1-.8*saturate(change))*saturate(accepted);
        current=lerp(current,oldColor,weight);
        float m=lerp(currentLog,oldMoments.r,weight),s=lerp(currentLog*currentLog,oldMoments.g,weight);
        historyLength=min(oldMoments.b+1,32);output.moments=float4(m,max(s,m*m),historyLength,accepted);
    }
    else output.moments=float4(currentLog,max(currentLog*currentLog+variance,currentLog*currentLog),historyLength,0);
    output.color=FiniteColor(current);return output;
}
#endif
