#version 450
#ifndef SCREEN_DENOISE_SPATIAL
#define SCREEN_DENOISE_SPATIAL 0
#endif
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
#if !SCREEN_DENOISE_SPATIAL
layout(location=1) out vec4 outputMoments;
layout(location=2) out vec4 outputGuide;
#endif
layout(set=0,binding=0) uniform texture2D normalImage;
layout(set=0,binding=1) uniform texture2D motionImage;
layout(set=0,binding=2) uniform texture2D sourceImage;
layout(set=0,binding=3) uniform texture2D historyImage;
layout(set=0,binding=4) uniform texture2D momentImage;
layout(set=0,binding=5) uniform texture2D guideImage;
layout(set=0,binding=7) uniform sampler pointSampler;
layout(set=0,binding=12) uniform Frame {mat4 view;mat4 projection;mat4 inverseProjection;mat4 viewProjection;vec4 size;vec4 ao;vec4 lighting;vec4 options;} frame;
layout(push_constant) uniform Pass {vec4 operation;};
vec4 Load(texture2D image,ivec2 p)
{
    ivec2 size=textureSize(sampler2D(image,pointSampler),0);
    return texelFetch(sampler2D(image,pointSampler),clamp(p,ivec2(0),size-1),0);
}
vec3 Normal(vec2 packed)
{
    vec2 f=packed*2-1;vec3 n=vec3(f,1-abs(f.x)-abs(f.y));
    if(n.z<0)n.xy=(1-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
    return normalize(n);
}
vec4 FiniteColor(vec4 value)
{
    if(any(isnan(value)) || any(isinf(value)))return vec4(0);
    return vec4(clamp(value.rgb,vec3(0),vec3(65504)),clamp(value.a,0,1));
}
float Luminance(vec3 c) {return dot(c,vec3(.2126,.7152,.0722));}
float LogLuminance(vec3 c) {return log2(1+max(Luminance(c),0));}
vec3 SetLogLuminance(vec3 c,float value)
{
    float l=Luminance(c);return l>1e-6?c*((exp2(max(value,0))-1)/l):vec3(0);
}
ivec2 Representative(ivec2 halfPixel)
{
    ivec2 dimensions=ivec2(frame.size.xy),base=halfPixel*2,selected=min(base,dimensions-1);
    float nearest=1e30;
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
    {
        ivec2 p=min(base+ivec2(x,y),dimensions-1);float depth=Load(motionImage,p).z;
        if(depth>0 && depth<nearest) {nearest=depth;selected=p;}
    }
    return selected;
}
void main()
{
    ivec2 pixel=ivec2(gl_FragCoord.xy);
#if SCREEN_DENOISE_SPATIAL
    vec4 guide=Load(guideImage,pixel),center=FiniteColor(Load(sourceImage,pixel));
    if(guide.r<=0) {outputColor=vec4(0);return;}
    vec4 moments=Load(momentImage,pixel);float centerLog=LogLuminance(center.rgb);
    float sigma=sqrt(max(moments.g-moments.r*moments.r,0));
    vec3 n=Normal(guide.gb);vec4 sum=vec4(0);float total=0;
    int step=int(operation.w+.5);
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
    {
        ivec2 q=pixel+ivec2(x,y)*step;vec4 g=Load(guideImage,q);
        if(g.r<=0)continue;
        vec4 color=FiniteColor(Load(sourceImage,q));
        float weight=float((x==0?2:1)*(y==0?2:1));
        weight*=exp(-abs(g.r-guide.r)/max(.01,guide.r*.01*float(step)));
        weight*=pow(max(dot(n,Normal(g.gb)),0),32);
        weight*=exp(-abs(g.a-guide.a)*16);
        weight*=exp(-abs(LogLuminance(color.rgb)-centerLog)/max(4*sigma,.001));
        if(operation.y>.5)weight*=exp(-abs(color.a-center.a)*4);
        sum+=color*weight;total+=weight;
    }
    outputColor=FiniteColor(total>1e-5?sum/total:center);
#else
    ivec2 fullPixel=Representative(pixel);vec4 motion=Load(motionImage,fullPixel),surface=Load(normalImage,fullPixel);
    vec4 current=FiniteColor(Load(sourceImage,pixel));
    outputGuide=vec4(max(motion.z,0),surface.xy,clamp(surface.z,0,1));
    if(motion.z<=0) {outputColor=vec4(0);outputMoments=vec4(0);outputGuide=vec4(0);return;}
    float mean=0,second=0;
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
    {
        float l=LogLuminance(FiniteColor(Load(sourceImage,pixel+ivec2(x,y))).rgb);
        mean+=l/9;second+=l*l/9;
    }
    float variance=max(second-mean*mean,0),currentLog=LogLuminance(current.rgb);
    vec4 oldColor=vec4(0),oldMoments=vec4(0);float accepted=0;
    // 이 이력은 이전 지터 격자에 저장되므로 TAA의 지터 차를 다시 빼지 않는다.
    vec2 currentUV=(vec2(fullPixel)+.5)*frame.size.zw,previousUV=currentUV-motion.xy;
    ivec2 size=textureSize(sampler2D(sourceImage,pointSampler),0);
    if(operation.z>.5 && motion.w>0 && all(greaterThanEqual(previousUV,vec2(0))) && all(lessThanEqual(previousUV,vec2(1))))
    {
        vec2 previousPixel=previousUV*vec2(size)-.5;ivec2 base=ivec2(floor(previousPixel));vec2 f=fract(previousPixel);
        vec3 n=Normal(surface.xy);
        for(int y=0;y<2;++y)for(int x=0;x<2;++x)
        {
            ivec2 q=base+ivec2(x,y);if(any(lessThan(q,ivec2(0))) || any(greaterThanEqual(q,size)))continue;
            vec4 guide=Load(guideImage,q);
            if(guide.r<=0 || abs(guide.r-motion.w)>max(.01,motion.w*.01) ||
                dot(n,Normal(guide.gb))<.9 || abs(guide.a-surface.z)>.1)continue;
            float w=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);
            oldColor+=FiniteColor(Load(historyImage,q))*w;oldMoments+=Load(momentImage,q)*w;accepted+=w;
        }
    }
    float historyLength=1;
    if(accepted>1e-4)
    {
        oldColor/=accepted;oldMoments/=accepted;
        float oldVariance=max(oldMoments.g-oldMoments.r*oldMoments.r,0);
        // 고정 밝기 상한 대신 현재 공간 통계와 누적 분산으로 고립 표본을 제한한다.
        float high=max(mean+2*sqrt(variance),oldMoments.r+3*sqrt(oldVariance));
        if(currentLog>high) {current.rgb=SetLogLuminance(current.rgb,high);currentLog=high;}
        float oldLog=LogLuminance(oldColor.rgb);
        float low=min(currentLog,mean-2*sqrt(variance));high=max(currentLog,mean+2*sqrt(variance));
        oldColor.rgb=SetLogLuminance(oldColor.rgb,clamp(oldLog,max(low,0),high));
        float change=abs(currentLog-oldMoments.r)/max(1,sqrt(variance+oldVariance));
        if(operation.y>.5)change=max(change,abs(current.a-oldColor.a));
        float weight=min(oldMoments.b/(oldMoments.b+1),.95)*(1-.8*clamp(change,0,1))*clamp(accepted,0,1);
        current=mix(current,oldColor,weight);
        float m=mix(currentLog,oldMoments.r,weight),s=mix(currentLog*currentLog,oldMoments.g,weight);
        historyLength=min(oldMoments.b+1,32);
        outputMoments=vec4(m,max(s,m*m),historyLength,accepted);
    }
    else outputMoments=vec4(currentLog,max(currentLog*currentLog+variance,currentLog*currentLog),historyLength,0);
    outputColor=FiniteColor(current);
#endif
}
