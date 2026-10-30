#include <metal_stdlib>
using namespace metal;
struct ScreenFrame {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4x4 viewProjection;float4 size;float4 ao;float4 lighting;float4 options;};
struct ScreenOutput {float4 position [[position]];float2 uv;};
#define SS_CONTEXT constant ScreenFrame& frame,constant float4& operation,texture2d<float> normalImage,texture2d<float> motionImage,texture2d<float> albedoImage,texture2d<float> hdrImage,texture2d<float> environmentImage,texture2d<float> sourceImage,texture2d<float> reflectionImage,sampler linearSampler
#define SS_INPUT frame,operation,normalImage,motionImage,albedoImage,hdrImage,environmentImage,sourceImage,reflectionImage,linearSampler
constant float PI=3.14159265359;
float4 geometry(texture2d<float> image,float2 p,SS_CONTEXT)
{
    int2 dimensions=int2(image.get_width(),image.get_height());
    return image.read(uint2(clamp(int2(p*float2(dimensions)),int2(0),dimensions-1)));
}
float3 decodeWorldNormal(float2 packed)
{
    float2 f=packed*2.0-1.0;
    float3 n=float3(f,1.0-abs(f.x)-abs(f.y));
    if(n.z<0)n.xy=(1.0-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);
    return normalize(n);
}
float3 normalWorld(float2 p,SS_CONTEXT) {return decodeWorldNormal(geometry(normalImage,p,SS_INPUT).xy);}
float3 normalView(float2 p,SS_CONTEXT) {return normalize(float3x3(frame.view[0].xyz,frame.view[1].xyz,frame.view[2].xyz)*normalWorld(p,SS_INPUT));}
float depthAt(float2 p,SS_CONTEXT) {return geometry(motionImage,p,SS_INPUT).z;}
float2 halfSurfaceUV(float2 p,SS_CONTEXT)
{
    // Match the surface used by ray generation, filtering, and upsampling.
    int2 dimensions=int2(frame.size.xy),halfDimensions=(dimensions+1)/2;
    int2 base=clamp(int2(p*float2(halfDimensions)),int2(0),halfDimensions-1)*2;
    int2 selected=min(base,dimensions-1);float nearest=1e30;
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
    {
        int2 pixel=min(base+int2(x,y),dimensions-1);
        float depth=motionImage.read(uint2(pixel)).z;
        if(depth>0.0 && depth<nearest) {nearest=depth;selected=pixel;}
    }
    return (float2(selected)+0.5)*frame.size.zw;
}
float3 reconstruct(float2 p,float depth,SS_CONTEXT)
{
    float2 ndc=p*float2(2,-2)+float2(-1,1);
    float4 a=frame.inverseProjection*float4(ndc,0,1),b=frame.inverseProjection*float4(ndc,1,1);
    a/=a.w;b/=b.w;
    return mix(a.xyz,b.xyz,(-depth-a.z)/(b.z-a.z));
}
float2 project(float3 p,SS_CONTEXT)
{
    float4 clip=frame.projection*float4(p,1);
    return clip.xy/max(clip.w,1e-5)*float2(0.5,-0.5)+0.5;
}
bool onScreen(float2 p,SS_CONTEXT) {return all(p>float2(0.001)) && all(p<float2(0.999));}
float randomAngle(float2 p,float frameIndex,SS_CONTEXT)
{
    return fract(sin(dot(floor(p*frame.size.xy),float2(12.9898,78.233))+frameIndex*0.75487766)*43758.5453)*2.0*PI;
}
float3x3 basis(float3 n,SS_CONTEXT)
{
    float3 t=normalize(cross(abs(n.z)<0.99?float3(0,0,1):float3(0,1,0),n));
    return float3x3(t,cross(n,t),n);
}
float4 hemisphereAO(float2 p,SS_CONTEXT)
{
    p=halfSurfaceUV(p,SS_INPUT);
    float depth=depthAt(p,SS_INPUT);
    float2 packedNormal=geometry(normalImage,p,SS_INPUT).xy;
    // R is AO; GBA preserve the exact representative surface for both filters.
    if(depth<=0.0 || frame.options.x<0.5)return float4(1.0,max(depth,0.0),packedNormal);
    float3 origin=reconstruct(p,depth,SS_INPUT),n=normalize(float3x3(frame.view[0].xyz,frame.view[1].xyz,frame.view[2].xyz)*decodeWorldNormal(packedNormal));float3x3 tbn=basis(n,SS_INPUT);
    float angle=randomAngle(p,operation.w,SS_INPUT),blocked=0.0;
    for(int i=0;i<12;++i)
    {
        float f=(float(i)+0.5)/12.0,a=angle+float(i)*2.39996323;
        float3 direction=tbn*float3(cos(a)*sqrt(f),sin(a)*sqrt(f),sqrt(1.0-f));
        float3 candidate=origin+direction*frame.ao.x*(0.12+0.88*f*f);
        float2 sampleUV=project(candidate,SS_INPUT);
        if(!onScreen(sampleUV,SS_INPUT) || candidate.z>=-0.01)continue;
        float sampledDepth=depthAt(sampleUV,SS_INPUT);
        float range=1.0-smoothstep(frame.ao.x*0.5,frame.ao.x*1.5,abs(sampledDepth-depth));
        if(sampledDepth>0.0)
            blocked+=range*smoothstep(0.0,frame.ao.z,-candidate.z-sampledDepth-frame.ao.z);
    }
    return float4(clamp(1.0-frame.ao.y*blocked/12.0,0.0,1.0),depth,packedNormal);
}
float4 bilateralAO(float2 p,SS_CONTEXT)
{
    float4 center=geometry(sourceImage,p,SS_INPUT);
    float depth=center.g;if(depth<=0.0)return float4(1.0,center.gba);
    float3x3 worldToView(frame.view[0].xyz,frame.view[1].xyz,frame.view[2].xyz);
    float3 n=normalize(worldToView*decodeWorldNormal(center.ba));float sum=0.0,weights=0.0;
    for(int i=-4;i<=4;++i)
    {
        float2 q=clamp(p+operation.yz*float(i),float2(0.0),float2(1.0));
        float4 sampleAO=geometry(sourceImage,q,SS_INPUT);
        float sampleDepth=sampleAO.g;if(sampleDepth<=0.0)continue;
        float3 sampleNormal=normalize(worldToView*decodeWorldNormal(sampleAO.ba));
        float weight=exp(-float(i*i)*0.125)*exp(-abs(depth-sampleDepth)/max(frame.ao.x*0.15,0.01))
            *pow(max(dot(n,sampleNormal),0.0),16.0);
        sum+=sampleAO.r*weight;weights+=weight;
    }
    return float4(weights>1e-5?sum/weights:1.0,center.gba);
}
bool trace(float3 origin,float3 direction,float distanceLimit,int steps,thread float2& hitUV,thread float& hitDistance,thread float& hitConfidence,SS_CONTEXT)
{
    float previousDistance=0.08,previousDelta=-1.0;
    hitUV=float2(0);hitDistance=0.0;hitConfidence=0.0;
    for(int i=1;i<=steps;++i)
    {
        float f=float(i)/float(steps),distance=0.08+distanceLimit*f*f;
        float3 candidate=origin+direction*distance;
        if(candidate.z>=-0.03)break;
        float2 q=project(candidate,SS_INPUT);if(!onScreen(q,SS_INPUT))break;
        float depth=depthAt(q,SS_INPUT),delta=-candidate.z-depth;
        if(depth>0.0 && delta>=0.0 && previousDelta<0.0)
        {
            float lo=previousDistance,hi=distance;
            for(int refine=0;refine<5;++refine)
            {
                float mid=(lo+hi)*0.5;float3 test=origin+direction*mid;float2 testUV=project(test,SS_INPUT);
                float testDepth=depthAt(testUV,SS_INPUT);
                if(testDepth>0.0 && -test.z>testDepth)hi=mid;else lo=mid;
            }
            float3 hit=origin+direction*hi;hitUV=project(hit,SS_INPUT);hitDistance=hi;
            float gap=-hit.z-depthAt(hitUV,SS_INPUT);
            float facing=dot(normalView(hitUV,SS_INPUT),-direction);
            if(gap>=0.0 && gap<frame.lighting.w && facing>0.05)
            {
                float edge=min(min(hitUV.x,1.0-hitUV.x),min(hitUV.y,1.0-hitUV.y));
                hitConfidence=(1.0-smoothstep(frame.lighting.w*0.5,frame.lighting.w,gap))
                    *smoothstep(0.05,0.25,facing)*smoothstep(0.0,0.08,edge);
                return true;
            }
        }
        previousDistance=distance;previousDelta=depth>0.0?delta:-1.0;
    }
    return false;
}
void indirectLighting(float2 p,thread float4& gi,thread float4& reflection,SS_CONTEXT)
{
    p=halfSurfaceUV(p,SS_INPUT);
    gi=float4(0);reflection=float4(0);float depth=depthAt(p,SS_INPUT);if(depth<=0.0)return;
    float3 origin=reconstruct(p,depth,SS_INPUT),n=normalView(p,SS_INPUT),v=normalize(-origin);
    float4 material=geometry(albedoImage,p,SS_INPUT),surface=geometry(normalImage,p,SS_INPUT);
    if(surface.z<0)return;
    float3x3 tbn=basis(n,SS_INPUT);float angle=randomAngle(p,frame.ao.w,SS_INPUT);
    if(frame.options.y>0.5 && operation.w<0.5 && material.a<0.99)
    {
        float3 radiance=float3(0);
        for(int ray=0;ray<4;++ray)
        {
            float f=(float(ray)+0.5)/4.0,a=angle+float(ray)*2.39996323;
            float3 direction=tbn*float3(cos(a)*sqrt(f),sin(a)*sqrt(f),sqrt(1.0-f));
            float2 hitUV;float distance,confidence;
            if(trace(origin+n*0.06,direction,frame.lighting.x,16,hitUV,distance,confidence,SS_INPUT))
                radiance+=hdrImage.sample(linearSampler,hitUV,level(0)).rgb*confidence/(1.0+distance*distance*0.05);
        }
        gi=float4(clamp(radiance*0.25*material.rgb*(1.0-material.a)*surface.w*frame.lighting.y,0.0f,65504.0f),1);
    }
    float roughness=surface.z;
    if(frame.options.z>0.5 && operation.w>0.5 && roughness<0.85)
    {
        float3 direction=reflect(-v,n);float2 hitUV;float distance,hitConfidence;
        if(trace(origin+n*0.06,direction,frame.lighting.x*2.0,32,hitUV,distance,hitConfidence,SS_INPUT))
        {
            float2 spread=frame.size.zw*roughness*roughness*24.0;
            float3 radiance=hdrImage.sample(linearSampler,hitUV,level(0)).rgb*0.4;
            radiance+=hdrImage.sample(linearSampler,hitUV+float2(spread.x,0),level(0)).rgb*0.15;
            radiance+=hdrImage.sample(linearSampler,hitUV-float2(spread.x,0),level(0)).rgb*0.15;
            radiance+=hdrImage.sample(linearSampler,hitUV+float2(0,spread.y),level(0)).rgb*0.15;
            radiance+=hdrImage.sample(linearSampler,hitUV-float2(0,spread.y),level(0)).rgb*0.15;
            float3 f0=mix(float3(0.04),material.rgb,material.a);
            float3 fresnel=f0+(1.0-f0)*pow(1.0-max(dot(n,v),0.0),5.0);
            float confidence=hitConfidence*(1.0-smoothstep(0.55,0.85,roughness))
                *(1.0-smoothstep(frame.lighting.x,frame.lighting.x*2.0,distance));
            confidence=clamp(confidence*frame.lighting.z,0.0,1.0);
            reflection=float4(clamp(radiance*fresnel*(1.0-0.6*roughness)*surface.w*confidence,0.0f,65504.0f),confidence);
        }
    }
}
float4 composite(float2 p,SS_CONTEXT)
{
    float4 hdr=hdrImage.sample(linearSampler,p,level(0));
    float depth=depthAt(p,SS_INPUT);if(depth<=0.0 || geometry(normalImage,p,SS_INPUT).z<0)return hdr;
    float3 n=normalView(p,SS_INPUT),gi=float3(0);float4 reflection=float4(0);float total=0.0;
    float2 dimensions=frame.options.y>0.5?float2(sourceImage.get_width(),sourceImage.get_height())
        :float2(reflectionImage.get_width(),reflectionImage.get_height());
    float2 pixel=p*dimensions-0.5,base=floor(pixel),fraction=fract(pixel);
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
    {
        float2 q=clamp((base+float2(x,y)+0.5)/dimensions,float2(0),float2(1));
        float2 surfaceUV=halfSurfaceUV(q,SS_INPUT);
        float sampleDepth=depthAt(surfaceUV,SS_INPUT);if(sampleDepth<=0.0)continue;
        float weight=(x==0?1.0-fraction.x:fraction.x)*(y==0?1.0-fraction.y:fraction.y)
            *exp(-abs(depth-sampleDepth)/max(0.05,depth*0.02))*pow(max(dot(n,normalView(surfaceUV,SS_INPUT)),0.0),16.0);
        if(frame.options.y>0.5)gi+=sourceImage.sample(linearSampler,q,level(0)).rgb*weight;
        if(frame.options.z>0.5)reflection+=reflectionImage.sample(linearSampler,q,level(0))*weight;
        total+=weight;
    }
    if(total>1e-5) {gi/=total;reflection/=total;}
    float3 environment=frame.options.z>0.5?environmentImage.sample(linearSampler,p,level(0)).rgb:float3(0);
    return float4(max(hdr.rgb+gi+reflection.rgb-environment*reflection.a,float3(0)),hdr.a);
}
fragment float4 screenSpaceFragment(ScreenOutput input [[stage_in]],
    constant ScreenFrame& frame [[buffer(12)]],constant float4& operation [[buffer(15)]],
    texture2d<float> normalImage [[texture(0)]],texture2d<float> motionImage [[texture(1)]],
    texture2d<float> albedoImage [[texture(2)]],texture2d<float> hdrImage [[texture(3)]],
    texture2d<float> environmentImage [[texture(4)]],texture2d<float> sourceImage [[texture(5)]],
    texture2d<float> reflectionImage [[texture(6)]],sampler linearSampler [[sampler(7)]])
{
    if(operation.x<0.5)return hemisphereAO(input.uv,SS_INPUT);
    if(operation.x<1.5)return bilateralAO(input.uv,SS_INPUT);
    return composite(input.uv,SS_INPUT);
}
fragment float4 screenSpaceLightingFragment(ScreenOutput input [[stage_in]],
    constant ScreenFrame& frame [[buffer(12)]],constant float4& operation [[buffer(15)]],
    texture2d<float> normalImage [[texture(0)]],texture2d<float> motionImage [[texture(1)]],
    texture2d<float> albedoImage [[texture(2)]],texture2d<float> hdrImage [[texture(3)]],
    texture2d<float> environmentImage [[texture(4)]],texture2d<float> sourceImage [[texture(5)]],
    texture2d<float> reflectionImage [[texture(6)]],sampler linearSampler [[sampler(7)]])
{
    float4 gi,reflection;indirectLighting(input.uv,gi,reflection,SS_INPUT);
    return operation.w>0.5?reflection:gi;
}
