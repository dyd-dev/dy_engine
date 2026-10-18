#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
layout(set=0,binding=0) uniform texture2D currentImage;
layout(set=0,binding=1) uniform texture2D motionImage;
layout(set=0,binding=2) uniform texture2D historyImage;
layout(set=0,binding=3) uniform texture2D reactiveImage;
layout(set=0,binding=8) uniform sampler imageSampler;
layout(push_constant) uniform Settings {vec4 temporal;vec4 sizeReject;vec4 jitter;mat4 skyCurrentToPreviousClip;} settings;

vec3 ToYCoCg(vec3 c)
{
    return vec3(dot(c,vec3(.25,.5,.25)),.5*c.r-.5*c.b,-.25*c.r+.5*c.g-.25*c.b);
}
vec3 ToRGB(vec3 c) {return vec3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
ivec2 Pixel(ivec2 p) {return clamp(p,ivec2(0),ivec2(settings.sizeReject.xy)-ivec2(1));}

vec4 CubicWeights(float f)
{
    float f2=f*f,f3=f2*f;
    return vec4(-.5*f3+f2-.5*f,1.5*f3-2.5*f2+1,-1.5*f3+2*f2+.5*f,.5*f3-.5*f2);
}
vec4 MomentWeights(float f)
{
    float g=1-f,f2=f*f,f3=f2*f;
    return vec4(g*g*g,3*f3-6*f2+4,-3*f3+3*f2+3*f+1,f3)/6;
}
// Reconstruct the jittered samples at the exact unjittered output center.
// Positive B-spline moments share that center and vary continuously with phase.
vec3 ReconstructCurrent(vec2 coordinate,out vec3 lower,out vec3 upper)
{
    vec2 position=coordinate*settings.sizeReject.xy-.5;
    ivec2 base=ivec2(floor(position));vec2 f=fract(position);
    vec4 wx=CubicWeights(f.x),wy=CubicWeights(f.y),mx=MomentWeights(f.x),my=MomentWeights(f.y);
    vec3 reconstructed=vec3(0),mean=vec3(0),second=vec3(0);
    vec3 coreMinimum=vec3(1e30),coreMaximum=vec3(-1e30);
    for(int y=0;y<4;++y) for(int x=0;x<4;++x)
    {
        vec3 c=max(texelFetch(sampler2D(currentImage,imageSampler),Pixel(base+ivec2(x-1,y-1)),0).rgb,vec3(0));
        reconstructed+=c*(wx[x]*wy[y]);
        vec3 encoded=ToYCoCg(c);float weight=mx[x]*my[y];
        mean+=encoded*weight;second+=encoded*encoded*weight;
        if(x>=1 && x<=2 && y>=1 && y<=2)
        {
            coreMinimum=min(coreMinimum,c);coreMaximum=max(coreMaximum,c);
        }
    }
    // Negative cubic lobes must not introduce colors outside the central footprint.
    reconstructed=clamp(reconstructed,coreMinimum,coreMaximum);
    vec3 deviation=sqrt(max(second-mean*mean,vec3(0))),center=ToYCoCg(reconstructed);
    lower=min(mean-1.5*deviation,center);upper=max(mean+1.5*deviation,center);
    return reconstructed;
}
vec3 ReconstructHistory(vec2 coordinate)
{
    vec2 position=coordinate*settings.sizeReject.xy-.5;
    vec2 base=floor(position),f=fract(position);
    vec4 wx=CubicWeights(f.x),wy=CubicWeights(f.y);
    vec3 weightX=vec3(wx.x,wx.y+wx.z,wx.w),weightY=vec3(wy.x,wy.y+wy.z,wy.w);
    vec3 px=vec3(base.x-.5,base.x+.5+wx.z/weightX.y,base.x+2.5)*settings.temporal.x;
    vec3 py=vec3(base.y-.5,base.y+.5+wy.z/weightY.y,base.y+2.5)*settings.temporal.y;
    vec3 result=vec3(0);
    // Merge the two positive middle coefficients into one bilinear sample.
    for(int y=0;y<3;++y) for(int x=0;x<3;++x)
        result+=textureLod(sampler2D(historyImage,imageSampler),vec2(px[x],py[y]),0).rgb*(weightX[x]*weightY[y]);
    return max(result,vec3(0));
}

void main()
{
    // The output/history grid is unjittered. Geometry and motion are sampled
    // in the current jittered grid, then reprojected into stable history UVs.
    vec2 outputUV=gl_FragCoord.xy*settings.temporal.xy;
    vec2 currentUV=outputUV+settings.jitter.xy;
    ivec2 pixel=Pixel(ivec2(currentUV*settings.sizeReject.xy));
    vec4 surface=texelFetch(sampler2D(motionImage,imageSampler),pixel,0);
    // A silhouette's foreground velocity, current depth and expected previous
    // depth must stay paired, including a center sample that currently sees sky.
    vec4 motion=surface;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
    {
        vec4 candidate=texelFetch(sampler2D(motionImage,imageSampler),Pixel(pixel+ivec2(x,y)),0);
        if(candidate.z>0 && (motion.z<=0 || candidate.z<motion.z)) motion=candidate;
    }
    bool geometry=motion.z>0;
    vec3 lower,upper;
    vec3 current=ReconstructCurrent(currentUV,lower,upper);
    vec3 result=current;
    if(settings.temporal.w>.5 && (!geometry || motion.w>0))
    {
        vec2 stableMotion=vec2(0),historyUV=vec2(-1);
        if(geometry)
        {
            stableMotion=motion.xy-settings.jitter.xy+settings.jitter.zw;
            historyUV=outputUV-stableMotion;
        }
        else
        {
            // Depthless sky shares current-image jitter with geometry, but its
            // previous position depends only on view rotation and projection.
            vec4 previousClip=settings.skyCurrentToPreviousClip*vec4(outputUV*vec2(2,-2)+vec2(-1,1),1,1);
            if(previousClip.w>0)
            {
                historyUV=previousClip.xy/previousClip.w*vec2(.5,-.5)+.5;
                stableMotion=outputUV-historyUV;
            }
        }
        vec2 margin=settings.temporal.xy*.5;
        if(all(greaterThanEqual(historyUV,margin)) && all(lessThanEqual(historyUV,vec2(1)-margin)))
        {
            ivec2 historyPixel=Pixel(ivec2(historyUV*settings.sizeReject.xy));
            float oldDepth=texelFetch(sampler2D(historyImage,imageSampler),historyPixel,0).a;
            float tolerance=max(settings.sizeReject.z,settings.sizeReject.w*motion.w);
            bool sameSurface=geometry ? oldDepth>0 && abs(oldDepth-motion.w)<=tolerance : oldDepth==0;
            if(sameSurface)
            {
                vec3 rawHistory=ReconstructHistory(historyUV);
                vec3 history=ToYCoCg(rawHistory);
                // Coverage alone is not a color change. Keep stable transparent
                // samples accumulated instead of displaying each raw jitter phase.
                // This ratio is invariant to a common HDR/exposure scale.
                float colorEnergy=dot(current+rawHistory,vec3(1));
                float relativeChange=colorEnergy>0?dot(abs(current-rawHistory),vec3(1))/colorEnergy:0;
                float colorResponse=smoothstep(.05,.25,relativeChange);
                ivec2 reactiveSize=textureSize(sampler2D(reactiveImage,imageSampler),0);
                ivec2 reactivePixel=ivec2(currentUV*vec2(reactiveSize));
                float reactive=0;
                for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
                {
                    ivec2 rp=clamp(reactivePixel+ivec2(x,y),ivec2(0),reactiveSize-ivec2(1));
                    reactive=max(reactive,texelFetch(sampler2D(reactiveImage,imageSampler),rp,0).r);
                }
                history=clamp(history,lower,upper);
                vec3 center=ToYCoCg(current);
                float speed=length(stableMotion*settings.sizeReject.xy);
                float weight=settings.temporal.z*mix(1,.65,clamp(speed/32,0,1));
                weight/=1+.5*abs(history.x-center.x)/max(center.x,.15);
                weight*=1-clamp(reactive,0,1)*colorResponse;
                result=ToRGB(mix(center,history,weight));
            }
        }
    }
    // Alpha carries scene depth only in this internal HDR history texture.
    outputColor=vec4(clamp(result,vec3(0),vec3(64000)),geometry ? motion.z : 0);
}
