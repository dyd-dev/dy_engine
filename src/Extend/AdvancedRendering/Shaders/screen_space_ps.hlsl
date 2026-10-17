#ifndef SCREEN_SPACE_LIGHTING
#define SCREEN_SPACE_LIGHTING 0
#endif
Texture2D<float4> normalImage:register(t0),motionImage:register(t1),albedoImage:register(t2),
    hdrImage:register(t3),environmentImage:register(t4),sourceImage:register(t5),reflectionImage:register(t6);
SamplerState linearSampler:register(s7);
cbuffer Frame:register(b12) {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4x4 viewProjection;float4 size;float4 ao;float4 lighting;float4 options;};
cbuffer Pass:register(b15) {float4 operation;};
static const float PI=3.14159265359;
float4 geometry(Texture2D<float4> image,float2 p)
{
    uint width,height;image.GetDimensions(width,height);int2 dimensions=int2(width,height);
    return image.Load(int3(clamp(int2(p*float2(dimensions)),int2(0,0),dimensions-1),0));
}
float3 decodeWorldNormal(float2 packed)
{
    float2 f=packed*2.0-1.0;
    float3 n=float3(f,1.0-abs(f.x)-abs(f.y));
    if(n.z<0)n.xy=(1.0-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);
    return normalize(n);
}
float3 normalWorld(float2 p) {return decodeWorldNormal(geometry(normalImage,p).xy);}
float3 normalView(float2 p) {return normalize(mul((float3x3)view,normalWorld(p)));}
float depthAt(float2 p) {return geometry(motionImage,p).z;}
float2 halfSurfaceUV(float2 p)
{
    // Match the surface used by ray generation, filtering, and upsampling.
    int2 dimensions=int2(size.xy),halfDimensions=(dimensions+1)/2;
    int2 base=clamp(int2(p*float2(halfDimensions)),int2(0,0),halfDimensions-1)*2;
    int2 selected=min(base,dimensions-1);float nearest=1e30;
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
    {
        int2 pixel=min(base+int2(x,y),dimensions-1);
        float depth=motionImage.Load(int3(pixel,0)).z;
        if(depth>0.0 && depth<nearest) {nearest=depth;selected=pixel;}
    }
    return (float2(selected)+0.5)*size.zw;
}
float3 reconstruct(float2 p,float depth)
{
    float2 ndc=p*float2(2,-2)+float2(-1,1);
    float4 a=mul(inverseProjection,float4(ndc,0,1)),b=mul(inverseProjection,float4(ndc,1,1));
    a/=a.w;b/=b.w;
    return lerp(a.xyz,b.xyz,(-depth-a.z)/(b.z-a.z));
}
float2 project(float3 p)
{
    float4 clip=mul(projection,float4(p,1));
    return clip.xy/max(clip.w,1e-5)*float2(0.5,-0.5)+0.5;
}
bool onScreen(float2 p) {return all(p>float2(0.001,0.001)) && all(p<float2(0.999,0.999));}
float randomAngle(float2 p,float frameIndex)
{
    return frac(sin(dot(floor(p*size.xy),float2(12.9898,78.233))+frameIndex*0.75487766)*43758.5453)*2.0*PI;
}
float3x3 basis(float3 n)
{
    float3 t=normalize(cross(abs(n.z)<0.99?float3(0,0,1):float3(0,1,0),n));
    return transpose(float3x3(t,cross(n,t),n));
}
float4 hemisphereAO(float2 p)
{
    p=halfSurfaceUV(p);
    float depth=depthAt(p);
    float2 packedNormal=geometry(normalImage,p).xy;
    // R is AO; GBA preserve the exact representative surface for both filters.
    if(depth<=0.0 || options.x<0.5)return float4(1.0,max(depth,0.0),packedNormal);
    float3 origin=reconstruct(p,depth),n=normalize(mul((float3x3)view,decodeWorldNormal(packedNormal)));float3x3 tbn=basis(n);
    float angle=randomAngle(p,operation.w),blocked=0.0;
    for(int i=0;i<12;++i)
    {
        float f=(float(i)+0.5)/12.0,a=angle+float(i)*2.39996323;
        float3 direction=mul(tbn,float3(cos(a)*sqrt(f),sin(a)*sqrt(f),sqrt(1.0-f)));
        float3 candidate=origin+direction*ao.x*(0.12+0.88*f*f);
        float2 sampleUV=project(candidate);
        if(!onScreen(sampleUV) || candidate.z>=-0.01)continue;
        float sampledDepth=depthAt(sampleUV);
        float range=1.0-smoothstep(ao.x*0.5,ao.x*1.5,abs(sampledDepth-depth));
        if(sampledDepth>0.0)
            blocked+=range*smoothstep(0.0,ao.z,-candidate.z-sampledDepth-ao.z);
    }
    return float4(clamp(1.0-ao.y*blocked/12.0,0.0,1.0),depth,packedNormal);
}
float4 bilateralAO(float2 p)
{
    float4 center=geometry(sourceImage,p);
    float depth=center.g;if(depth<=0.0)return float4(1.0,center.gba);
    float3 n=normalize(mul((float3x3)view,decodeWorldNormal(center.ba)));float sum=0.0,weights=0.0;
    for(int i=-4;i<=4;++i)
    {
        float2 q=clamp(p+operation.yz*float(i),float2(0.0),float2(1.0));
        float4 sampleAO=geometry(sourceImage,q);
        float sampleDepth=sampleAO.g;if(sampleDepth<=0.0)continue;
        float3 sampleNormal=normalize(mul((float3x3)view,decodeWorldNormal(sampleAO.ba)));
        float weight=exp(-float(i*i)*0.125)*exp(-abs(depth-sampleDepth)/max(ao.x*0.15,0.01))
            *pow(max(dot(n,sampleNormal),0.0),16.0);
        sum+=sampleAO.r*weight;weights+=weight;
    }
    return float4(weights>1e-5?sum/weights:1.0,center.gba);
}
bool trace(float3 origin,float3 direction,float distanceLimit,int steps,out float2 hitUV,out float hitDistance,out float hitConfidence)
{
    float previousDistance=0.08,previousDelta=-1.0;
    hitUV=float2(0);hitDistance=0.0;hitConfidence=0.0;
    for(int i=1;i<=steps;++i)
    {
        float f=float(i)/float(steps),distance=0.08+distanceLimit*f*f;
        float3 candidate=origin+direction*distance;
        if(candidate.z>=-0.03)break;
        float2 q=project(candidate);if(!onScreen(q))break;
        float depth=depthAt(q),delta=-candidate.z-depth;
        if(depth>0.0 && delta>=0.0 && previousDelta<0.0)
        {
            float lo=previousDistance,hi=distance;
            for(int refine=0;refine<5;++refine)
            {
                float mid=(lo+hi)*0.5;float3 test=origin+direction*mid;float2 testUV=project(test);
                float testDepth=depthAt(testUV);
                if(testDepth>0.0 && -test.z>testDepth)hi=mid;else lo=mid;
            }
            float3 hit=origin+direction*hi;hitUV=project(hit);hitDistance=hi;
            float gap=-hit.z-depthAt(hitUV);
            float facing=dot(normalView(hitUV),-direction);
            if(gap>=0.0 && gap<lighting.w && facing>0.05)
            {
                float edge=min(min(hitUV.x,1.0-hitUV.x),min(hitUV.y,1.0-hitUV.y));
                hitConfidence=(1.0-smoothstep(lighting.w*0.5,lighting.w,gap))
                    *smoothstep(0.05,0.25,facing)*smoothstep(0.0,0.08,edge);
                return true;
            }
        }
        previousDistance=distance;previousDelta=depth>0.0?delta:-1.0;
    }
    return false;
}
void indirectLighting(float2 p,out float4 gi,out float4 reflection)
{
    p=halfSurfaceUV(p);
    gi=float4(0);reflection=float4(0);float depth=depthAt(p);if(depth<=0.0)return;
    float3 origin=reconstruct(p,depth),n=normalView(p),v=normalize(-origin);
    float4 material=geometry(albedoImage,p),surface=geometry(normalImage,p);
    if(surface.z<0)return;
    float3x3 tbn=basis(n);float angle=randomAngle(p,ao.w);
    if(options.y>0.5 && operation.w<0.5 && material.a<0.99)
    {
        float3 radiance=float3(0);
        for(int ray=0;ray<4;++ray)
        {
            float f=(float(ray)+0.5)/4.0,a=angle+float(ray)*2.39996323;
            float3 direction=mul(tbn,float3(cos(a)*sqrt(f),sin(a)*sqrt(f),sqrt(1.0-f)));
            float2 hitUV;float distance,confidence;
            if(trace(origin+n*0.06,direction,lighting.x,16,hitUV,distance,confidence))
                radiance+=hdrImage.SampleLevel(linearSampler,hitUV,0).rgb*confidence/(1.0+distance*distance*0.05);
        }
        gi=float4(clamp(radiance*0.25*material.rgb*(1.0-material.a)*surface.w*lighting.y,0,65504),1);
    }
    float roughness=surface.z;
    if(options.z>0.5 && operation.w>0.5 && roughness<0.85)
    {
        float3 direction=reflect(-v,n);float2 hitUV;float distance,hitConfidence;
        if(trace(origin+n*0.06,direction,lighting.x*2.0,32,hitUV,distance,hitConfidence))
        {
            float2 spread=size.zw*roughness*roughness*24.0;
            float3 radiance=hdrImage.SampleLevel(linearSampler,hitUV,0).rgb*0.4;
            radiance+=hdrImage.SampleLevel(linearSampler,hitUV+float2(spread.x,0),0).rgb*0.15;
            radiance+=hdrImage.SampleLevel(linearSampler,hitUV-float2(spread.x,0),0).rgb*0.15;
            radiance+=hdrImage.SampleLevel(linearSampler,hitUV+float2(0,spread.y),0).rgb*0.15;
            radiance+=hdrImage.SampleLevel(linearSampler,hitUV-float2(0,spread.y),0).rgb*0.15;
            float3 f0=lerp(float3(0.04),material.rgb,material.a);
            float3 fresnel=f0+(1.0-f0)*pow(1.0-max(dot(n,v),0.0),5.0);
            float confidence=hitConfidence*(1.0-smoothstep(0.55,0.85,roughness))
                *(1.0-smoothstep(lighting.x,lighting.x*2.0,distance));
            confidence=clamp(confidence*lighting.z,0.0,1.0);
            reflection=float4(clamp(radiance*fresnel*(1.0-0.6*roughness)*surface.w*confidence,0,65504),confidence);
        }
    }
}
float4 composite(float2 p)
{
    float4 hdr=hdrImage.SampleLevel(linearSampler,p,0);
    float depth=depthAt(p);if(depth<=0.0 || geometry(normalImage,p).z<0)return hdr;
    float3 n=normalView(p),gi=float3(0);float4 reflection=float4(0);float total=0.0;
    uint width,height;
    if(options.y>0.5)sourceImage.GetDimensions(width,height);else reflectionImage.GetDimensions(width,height);
    float2 dimensions=float2(width,height);
    float2 pixel=p*dimensions-0.5,base=floor(pixel),fraction=frac(pixel);
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
    {
        float2 q=clamp((base+float2(x,y)+0.5)/dimensions,float2(0),float2(1));
        float2 surfaceUV=halfSurfaceUV(q);
        float sampleDepth=depthAt(surfaceUV);if(sampleDepth<=0.0)continue;
        float weight=(x==0?1.0-fraction.x:fraction.x)*(y==0?1.0-fraction.y:fraction.y)
            *exp(-abs(depth-sampleDepth)/max(0.05,depth*0.02))*pow(max(dot(n,normalView(surfaceUV)),0.0),16.0);
        if(options.y>0.5)gi+=sourceImage.SampleLevel(linearSampler,q,0).rgb*weight;
        if(options.z>0.5)reflection+=reflectionImage.SampleLevel(linearSampler,q,0)*weight;
        total+=weight;
    }
    if(total>1e-5) {gi/=total;reflection/=total;}
    float3 environment=options.z>0.5?environmentImage.SampleLevel(linearSampler,p,0).rgb:float3(0);
    return float4(max(hdr.rgb+gi+reflection.rgb-environment*reflection.a,float3(0)),hdr.a);
}
struct Input {float4 position:SV_Position;float2 uv:TEXCOORD0;};
#if SCREEN_SPACE_LIGHTING
float4 main(Input input):SV_Target0
{
    float4 gi,reflection;indirectLighting(input.uv,gi,reflection);
    return operation.w>0.5?reflection:gi;
}
#else
float4 main(Input input):SV_Target
{
    if(operation.x<0.5)return hemisphereAO(input.uv);
    if(operation.x<1.5)return bilateralAO(input.uv);
    return composite(input.uv);
}
#endif
