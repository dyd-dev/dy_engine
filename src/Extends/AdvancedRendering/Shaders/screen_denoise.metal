#include <metal_stdlib>
using namespace metal;
struct DenoiseVertex {float4 position [[position]];float2 uv;};
struct DenoiseFrame {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4x4 viewProjection;float4 size;float4 ao;float4 lighting;float4 options;};
struct DenoiseOutput {float4 color [[color(0)]];float4 moments [[color(1)]];float4 guide [[color(2)]];};
#define DN_CONTEXT constant DenoiseFrame& frame,constant float4& operation,texture2d<float> normalImage,texture2d<float> motionImage,texture2d<float> sourceImage,texture2d<float> historyImage,texture2d<float> momentImage,texture2d<float> guideImage
#define DN_INPUT frame,operation,normalImage,motionImage,sourceImage,historyImage,momentImage,guideImage
float4 DnLoad(texture2d<float> image,int2 p)
{
    return image.read(uint2(clamp(p,int2(0),int2(image.get_width(),image.get_height())-1)));
}
float3 DnNormal(float2 packed)
{
    float2 f=packed*2-1;float3 n=float3(f,1-abs(f.x)-abs(f.y));
    if(n.z<0)n.xy=(1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);
    return normalize(n);
}
float4 DnFinite(float4 value)
{
    if(!all(isfinite(value)))return float4(0);
    return float4(clamp(value.rgb,0.0f,65504.0f),clamp(value.a,0.0f,1.0f));
}
float DnLuminance(float3 c) {return dot(c,float3(.2126f,.7152f,.0722f));}
float DnLog(float3 c) {return log2(1+max(DnLuminance(c),0.0f));}
float3 DnSetLog(float3 c,float value)
{
    float l=DnLuminance(c);return l>1e-6f?c*((exp2(max(value,0.0f))-1)/l):float3(0);
}
int2 DnRepresentative(int2 halfPixel,constant DenoiseFrame& frame,texture2d<float> motionImage)
{
    int2 dimensions=int2(frame.size.xy),base=halfPixel*2,selected=min(base,dimensions-1);float nearest=1e30f;
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
    {
        int2 p=min(base+int2(x,y),dimensions-1);float depth=DnLoad(motionImage,p).z;
        if(depth>0 && depth<nearest) {nearest=depth;selected=p;}
    }
    return selected;
}
DenoiseOutput DnTemporal(int2 pixel,DN_CONTEXT)
{
    DenoiseOutput output;int2 fullPixel=DnRepresentative(pixel,frame,motionImage);
    float4 motion=DnLoad(motionImage,fullPixel),surface=DnLoad(normalImage,fullPixel),current=DnFinite(DnLoad(sourceImage,pixel));
    output.guide=float4(max(motion.z,0.0f),surface.xy,clamp(surface.z,0.0f,1.0f));
    if(motion.z<=0) {output.color=0;output.moments=0;output.guide=0;return output;}
    float currentLog=DnLog(current.rgb),mean=0,second=0;
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
    {
        float l=currentLog;
        if(x!=0 || y!=0)l=DnLog(DnFinite(DnLoad(sourceImage,pixel+int2(x,y))).rgb);
        mean+=l/9;second+=l*l/9;
    }
    float variance=max(second-mean*mean,0.0f);
    float4 oldColor=0,oldMoments=0;float accepted=0;
    float2 currentUV=(float2(fullPixel)+.5f)*frame.size.zw,previousUV=currentUV-motion.xy;
    int2 dimensions=int2(sourceImage.get_width(),sourceImage.get_height());
    if(operation.z>.5f && motion.w>0 && all(previousUV>=float2(0)) && all(previousUV<=float2(1)))
    {
        float2 previousPixel=previousUV*float2(dimensions)-.5f;int2 base=int2(floor(previousPixel));float2 f=fract(previousPixel);
        float3 n=DnNormal(surface.xy);
        for(int y=0;y<2;++y)for(int x=0;x<2;++x)
        {
            int2 q=base+int2(x,y);if(any(q<int2(0)) || any(q>=dimensions))continue;
            float4 guide=DnLoad(guideImage,q);
            if(guide.r<=0 || abs(guide.r-motion.w)>max(.01f,motion.w*.01f) ||
                dot(n,DnNormal(guide.gb))<.9f || abs(guide.a-surface.z)>.1f)continue;
            float weight=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);
            oldColor+=DnFinite(DnLoad(historyImage,q))*weight;oldMoments+=DnLoad(momentImage,q)*weight;accepted+=weight;
        }
    }
    float historyLength=1;
    if(accepted>1e-4f)
    {
        oldColor/=accepted;oldMoments/=accepted;float oldVariance=max(oldMoments.g-oldMoments.r*oldMoments.r,0.0f);
        float high=max(mean+2*sqrt(variance),oldMoments.r+3*sqrt(oldVariance));
        if(currentLog>high) {current.rgb=DnSetLog(current.rgb,high);currentLog=high;}
        float oldLog=DnLog(oldColor.rgb),low=min(currentLog,mean-2*sqrt(variance));high=max(currentLog,mean+2*sqrt(variance));
        oldColor.rgb=DnSetLog(oldColor.rgb,clamp(oldLog,max(low,0.0f),high));
        float change=abs(currentLog-oldMoments.r)/max(1.0f,sqrt(variance+oldVariance));
        if(operation.y>.5f)change=max(change,abs(current.a-oldColor.a));
        float weight=min(oldMoments.b/(oldMoments.b+1),.95f)*(1-.8f*clamp(change,0.0f,1.0f))*clamp(accepted,0.0f,1.0f);
        current=mix(current,oldColor,weight);
        float m=mix(currentLog,oldMoments.r,weight),s=mix(currentLog*currentLog,oldMoments.g,weight);
        historyLength=min(oldMoments.b+1,32.0f);output.moments=float4(m,max(s,m*m),historyLength,accepted);
    }
    else output.moments=float4(currentLog,max(currentLog*currentLog+variance,currentLog*currentLog),historyLength,0);
    output.color=DnFinite(current);return output;
}
float4 DnSpatial(int2 pixel,DN_CONTEXT)
{
    float4 guide=DnLoad(guideImage,pixel),center=DnFinite(DnLoad(sourceImage,pixel));
    if(guide.r<=0)return float4(0);
    float4 moments=DnLoad(momentImage,pixel);float centerLog=DnLog(center.rgb);
    float sigma=sqrt(max(moments.g-moments.r*moments.r,0.0f));
    float3 n=DnNormal(guide.gb);float4 sum=0;float total=0;int step=int(operation.w+.5f);
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
    {
        bool centerTap=x==0 && y==0;
        int2 q=pixel+int2(x,y)*step;float4 g=guide;
        if(!centerTap)g=DnLoad(guideImage,q);
        if(g.r<=0)continue;
        float4 color=center;float3 sampleNormal=n;float sampleLog=centerLog;
        if(!centerTap)
        {
            color=DnFinite(DnLoad(sourceImage,q));sampleNormal=DnNormal(g.gb);sampleLog=DnLog(color.rgb);
        }
        float weight=float((x==0?2:1)*(y==0?2:1));
        weight*=exp(-abs(g.r-guide.r)/max(.01f,guide.r*.01f*float(step)));
        weight*=pow(max(dot(n,sampleNormal),0.0f),32.0f)*exp(-abs(g.a-guide.a)*16);
        weight*=exp(-abs(sampleLog-centerLog)/max(4*sigma,.001f));
        if(operation.y>.5f)weight*=exp(-abs(color.a-center.a)*4);
        sum+=color*weight;total+=weight;
    }
    return DnFinite(total>1e-5f?sum/total:center);
}
fragment DenoiseOutput screenDenoiseTemporal(DenoiseVertex input [[stage_in]],
    texture2d<float> normalImage [[texture(0)]],texture2d<float> motionImage [[texture(1)]],
    texture2d<float> sourceImage [[texture(2)]],texture2d<float> historyImage [[texture(3)]],
    texture2d<float> momentImage [[texture(4)]],texture2d<float> guideImage [[texture(5)]],
    sampler pointSampler [[sampler(7)]],constant DenoiseFrame& frame [[buffer(12)]],constant float4& operation [[buffer(15)]])
{
    return DnTemporal(int2(input.position.xy),DN_INPUT);
}
fragment float4 screenDenoiseSpatial(DenoiseVertex input [[stage_in]],
    texture2d<float> normalImage [[texture(0)]],texture2d<float> motionImage [[texture(1)]],
    texture2d<float> sourceImage [[texture(2)]],texture2d<float> historyImage [[texture(3)]],
    texture2d<float> momentImage [[texture(4)]],texture2d<float> guideImage [[texture(5)]],
    sampler pointSampler [[sampler(7)]],constant DenoiseFrame& frame [[buffer(12)]],constant float4& operation [[buffer(15)]])
{
    return DnSpatial(int2(input.position.xy),DN_INPUT);
}
