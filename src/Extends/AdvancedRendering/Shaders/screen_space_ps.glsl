#version 450
#ifndef SCREEN_DEPTH_COPY
#define SCREEN_DEPTH_COPY 0
#endif
#if SCREEN_DEPTH_COPY
layout(location=0) out float result;
layout(set=0,binding=1) uniform texture2D motionImage;
layout(set=0,binding=7) uniform sampler linearSampler;
void main()
{
    result=texelFetch(sampler2D(motionImage,linearSampler),ivec2(gl_FragCoord.xy),0).z;
}
#else
#ifndef SCREEN_SPACE_LIGHTING
#define SCREEN_SPACE_LIGHTING 0
#endif
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(set=0,binding=0) uniform texture2D normalImage;
layout(set=0,binding=1) uniform texture2D motionImage;
layout(set=0,binding=2) uniform texture2D albedoImage;
layout(set=0,binding=3) uniform texture2D hdrImage;
layout(set=0,binding=4) uniform texture2D environmentImage;
layout(set=0,binding=5) uniform texture2D sourceImage;
layout(set=0,binding=6) uniform texture2D reflectionImage;
layout(set=0,binding=7) uniform sampler linearSampler;
layout(set=0,binding=9) uniform texture2D depthImage;
layout(set=0,binding=12) uniform Frame {mat4 view;mat4 projection;mat4 inverseProjection;mat4 viewProjection;vec4 size;vec4 ao;vec4 lighting;vec4 options;} frame;
layout(push_constant) uniform Pass {vec4 operation;};
const float PI=3.14159265359;
vec4 geometry(texture2D image,vec2 p)
{
    ivec2 dimensions=textureSize(sampler2D(image,linearSampler),0);
    return texelFetch(sampler2D(image,linearSampler),clamp(ivec2(p*vec2(dimensions)),ivec2(0),dimensions-1),0);
}
vec3 decodeWorldNormal(vec2 packed)
{
    vec2 f=packed*2.0-1.0;
    vec3 n=vec3(f,1.0-abs(f.x)-abs(f.y));
    if(n.z<0)n.xy=(1.0-abs(n.yx))*vec2(n.x>=0?1:-1,n.y>=0?1:-1);
    return normalize(n);
}
vec3 normalWorld(vec2 p) {return decodeWorldNormal(geometry(normalImage,p).xy);}
vec3 normalView(vec2 p) {return normalize(mat3(frame.view)*normalWorld(p));}
float depthAt(vec2 p) {return operation.z>0.5?geometry(depthImage,p).r:geometry(motionImage,p).z;}
vec2 halfSurfaceUV(vec2 p)
{
    // A half-resolution texel represents an actual 2x2 full-resolution
    // footprint. Use one visible surface consistently for all its attributes.
    ivec2 dimensions=ivec2(frame.size.xy),halfDimensions=(dimensions+1)/2;
    ivec2 base=clamp(ivec2(p*vec2(halfDimensions)),ivec2(0),halfDimensions-1)*2;
    ivec2 selected=min(base,dimensions-1);float nearest=1e30;
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
    {
        ivec2 pixel=min(base+ivec2(x,y),dimensions-1);
        float depth=operation.z>0.5?texelFetch(sampler2D(depthImage,linearSampler),pixel,0).r:
            texelFetch(sampler2D(motionImage,linearSampler),pixel,0).z;
        if(depth>0.0 && depth<nearest) {nearest=depth;selected=pixel;}
    }
    return (vec2(selected)+0.5)*frame.size.zw;
}
vec3 reconstruct(vec2 p,float depth)
{
    vec2 ndc=p*vec2(2,-2)+vec2(-1,1);
    vec4 a=frame.inverseProjection*vec4(ndc,0,1),b=frame.inverseProjection*vec4(ndc,1,1);
    a/=a.w;b/=b.w;
    return mix(a.xyz,b.xyz,(-depth-a.z)/(b.z-a.z));
}
vec2 project(vec3 p)
{
    vec4 clip=frame.projection*vec4(p,1);
    return clip.xy/max(clip.w,1e-5)*vec2(0.5,-0.5)+0.5;
}
vec2 projectRay(vec3 p,bool sparseProjection)
{
    if(!sparseProjection)return project(p);
    vec2 clip=vec2(frame.projection[0].x*p.x+frame.projection[2].x*p.z,
        frame.projection[1].y*p.y+frame.projection[2].y*p.z);
    return clip/max(-p.z,1e-5)*vec2(0.5,-0.5)+0.5;
}
bool onScreen(vec2 p) {return all(greaterThan(p,vec2(0.001))) && all(lessThan(p,vec2(0.999)));}
float randomAngle(vec2 p,float frameIndex)
{
    return fract(sin(dot(floor(p*frame.size.xy),vec2(12.9898,78.233))+frameIndex*0.75487766)*43758.5453)*2.0*PI;
}
mat3 basis(vec3 n)
{
    vec3 t=normalize(cross(abs(n.z)<0.99?vec3(0,0,1):vec3(0,1,0),n));
    return mat3(t,cross(n,t),n);
}
vec4 hemisphereAO(vec2 p)
{
    p=halfSurfaceUV(p);
    float depth=depthAt(p);
    vec2 packedNormal=geometry(normalImage,p).xy;
    // R is AO; GBA preserve the exact representative surface for both filters.
    if(depth<=0.0 || frame.options.x<0.5)return vec4(1.0,max(depth,0.0),packedNormal);
    vec3 origin=reconstruct(p,depth),n=normalize(mat3(frame.view)*decodeWorldNormal(packedNormal));mat3 tbn=basis(n);
    float angle=randomAngle(p,operation.w),blocked=0.0;
    for(int i=0;i<12;++i)
    {
        float f=(float(i)+0.5)/12.0,a=angle+float(i)*2.39996323;
        vec3 direction=tbn*vec3(cos(a)*sqrt(f),sin(a)*sqrt(f),sqrt(1.0-f));
        vec3 candidate=origin+direction*frame.ao.x*(0.12+0.88*f*f);
        vec2 sampleUV=project(candidate);
        if(!onScreen(sampleUV) || candidate.z>=-0.01)continue;
        float sampledDepth=depthAt(sampleUV);
        float range=1.0-smoothstep(frame.ao.x*0.5,frame.ao.x*1.5,abs(sampledDepth-depth));
        if(sampledDepth>0.0)
            blocked+=range*smoothstep(0.0,frame.ao.z,-candidate.z-sampledDepth-frame.ao.z);
    }
    return vec4(clamp(1.0-frame.ao.y*blocked/12.0,0.0,1.0),depth,packedNormal);
}
vec4 bilateralAO(vec2 p)
{
    vec4 center=geometry(sourceImage,p);
    float depth=center.g;if(depth<=0.0)return vec4(1.0,center.gba);
    vec3 n=normalize(mat3(frame.view)*decodeWorldNormal(center.ba));float sum=0.0,weights=0.0;
    for(int i=-4;i<=4;++i)
    {
        vec2 q=clamp(p+operation.yz*float(i),vec2(0.0),vec2(1.0));
        vec4 sampleAO=geometry(sourceImage,q);
        float sampleDepth=sampleAO.g;if(sampleDepth<=0.0)continue;
        vec3 sampleNormal=normalize(mat3(frame.view)*decodeWorldNormal(sampleAO.ba));
        float weight=exp(-float(i*i)*0.125)*exp(-abs(depth-sampleDepth)/max(frame.ao.x*0.15,0.01))
            *pow(max(dot(n,sampleNormal),0.0),16.0);
        sum+=sampleAO.r*weight;weights+=weight;
    }
    return vec4(weights>1e-5?sum/weights:1.0,center.gba);
}
bool trace(vec3 origin,vec3 direction,float distanceLimit,int steps,out vec2 hitUV,out float hitDistance,out float hitConfidence)
{
    // Bound every origin + direction * distance before removing zero products.
    bool sparseProjection=operation.y>0.5 && all(lessThanEqual(abs(origin),vec3(1e10))) &&
        all(lessThanEqual(abs(direction),vec3(2))) && distanceLimit>=0 && distanceLimit<=1e10;
    float previousDistance=0.08,previousDelta=-1.0;
    hitUV=vec2(0);hitDistance=0.0;hitConfidence=0.0;
    for(int i=1;i<=steps;++i)
    {
        float f=float(i)/float(steps),distance=0.08+distanceLimit*f*f;
        vec3 candidate=origin+direction*distance;
        if(candidate.z>=-0.03)break;
        vec2 q=projectRay(candidate,sparseProjection);if(!onScreen(q))break;
        float depth=depthAt(q),delta=-candidate.z-depth;
        if(depth>0.0 && delta>=0.0 && previousDelta<0.0)
        {
            float lo=previousDistance,hi=distance;
            // Keep the exact depth sample belonging to the current upper endpoint.
            vec2 hiUV=q;float hiDepth=depth;
            for(int refine=0;refine<5;++refine)
            {
                float mid=(lo+hi)*0.5;vec3 test=origin+direction*mid;vec2 testUV=projectRay(test,sparseProjection);
                float testDepth=depthAt(testUV);
                if(testDepth>0.0 && -test.z>testDepth) {hi=mid;hiUV=testUV;hiDepth=testDepth;}else lo=mid;
            }
            vec3 hit=origin+direction*hi;hitUV=hiUV;hitDistance=hi;
            float gap=-hit.z-hiDepth;
            if(gap>=0.0 && gap<frame.lighting.w)
            {
                // Rejected depth crossings do not need a normal fetch or decode.
                float facing=dot(normalView(hitUV),-direction);
                if(facing>0.05)
                {
                    float edge=min(min(hitUV.x,1.0-hitUV.x),min(hitUV.y,1.0-hitUV.y));
                    hitConfidence=(1.0-smoothstep(frame.lighting.w*0.5,frame.lighting.w,gap))
                        *smoothstep(0.05,0.25,facing)*smoothstep(0.0,0.08,edge);
                    return true;
                }
            }
        }
        previousDistance=distance;previousDelta=depth>0.0?delta:-1.0;
    }
    return false;
}
void indirectLighting(vec2 p,out vec4 gi,out vec4 reflection)
{
    p=halfSurfaceUV(p);
    gi=vec4(0);reflection=vec4(0);float depth=depthAt(p);if(depth<=0.0)return;
    vec4 surface=geometry(normalImage,p);
    if(surface.z<0)return;
    vec4 material=geometry(albedoImage,p);
    float roughness=surface.z;
    bool traceGI=frame.options.y>0.5 && operation.w<0.5 && material.a<0.99;
    bool traceReflection=frame.options.z>0.5 && operation.w>0.5 && roughness<0.85;
    if(!traceGI && !traceReflection)return;
    vec3 origin=reconstruct(p,depth),n=normalize(mat3(frame.view)*decodeWorldNormal(surface.xy));
    if(traceGI)
    {
        mat3 tbn=basis(n);float angle=randomAngle(p,frame.ao.w);
        vec3 radiance=vec3(0);
        for(int ray=0;ray<4;++ray)
        {
            float f=(float(ray)+0.5)/4.0,a=angle+float(ray)*2.39996323;
            vec3 direction=tbn*vec3(cos(a)*sqrt(f),sin(a)*sqrt(f),sqrt(1.0-f));
            vec2 hitUV;float distance,confidence;
            if(trace(origin+n*0.06,direction,frame.lighting.x,16,hitUV,distance,confidence))
                radiance+=texture(sampler2D(hdrImage,linearSampler),hitUV).rgb*confidence/(1.0+distance*distance*0.05);
        }
        gi=vec4(clamp(radiance*0.25*material.rgb*(1.0-material.a)*surface.w*frame.lighting.y,vec3(0),vec3(65504)),1);
    }
    if(traceReflection)
    {
        vec3 v=normalize(-origin);
        vec3 direction=reflect(-v,n);vec2 hitUV;float distance,hitConfidence;
        if(trace(origin+n*0.06,direction,frame.lighting.x*2.0,32,hitUV,distance,hitConfidence))
        {
            vec2 spread=frame.size.zw*roughness*roughness*24.0;
            vec3 radiance=texture(sampler2D(hdrImage,linearSampler),hitUV).rgb*0.4;
            radiance+=texture(sampler2D(hdrImage,linearSampler),hitUV+vec2(spread.x,0)).rgb*0.15;
            radiance+=texture(sampler2D(hdrImage,linearSampler),hitUV-vec2(spread.x,0)).rgb*0.15;
            radiance+=texture(sampler2D(hdrImage,linearSampler),hitUV+vec2(0,spread.y)).rgb*0.15;
            radiance+=texture(sampler2D(hdrImage,linearSampler),hitUV-vec2(0,spread.y)).rgb*0.15;
            vec3 f0=mix(vec3(0.04),material.rgb,material.a);
            vec3 fresnel=f0+(1.0-f0)*pow(1.0-max(dot(n,v),0.0),5.0);
            float confidence=hitConfidence*(1.0-smoothstep(0.55,0.85,roughness))
                *(1.0-smoothstep(frame.lighting.x,frame.lighting.x*2.0,distance));
            confidence=clamp(confidence*frame.lighting.z,0.0,1.0);
            reflection=vec4(clamp(radiance*fresnel*(1.0-0.6*roughness)*surface.w*confidence,vec3(0),vec3(65504)),confidence);
        }
    }
}
vec4 composite(vec2 p)
{
    vec4 hdr=texture(sampler2D(hdrImage,linearSampler),p);
    float depth=depthAt(p);if(depth<=0.0 || geometry(normalImage,p).z<0)return hdr;
    vec3 n=normalView(p),gi=vec3(0);vec4 reflection=vec4(0);float total=0.0;
    vec2 dimensions=frame.options.y>0.5
        ?vec2(textureSize(sampler2D(sourceImage,linearSampler),0))
        :vec2(textureSize(sampler2D(reflectionImage,linearSampler),0));
    vec2 pixel=p*dimensions-0.5,base=floor(pixel),fraction=fract(pixel);
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
    {
        vec2 q=clamp((base+vec2(x,y)+0.5)/dimensions,vec2(0),vec2(1));
        float sampleDepth=0.0;vec3 sampleNormal=vec3(0);
        bool guided=false;
        if(operation.w>0.5)
        {
            // The current denoise guide stores the same representative depth and packed normal.
            vec4 guide=geometry(albedoImage,q);
            if(guide.r>0.0)
            {
                sampleDepth=guide.r;
                sampleNormal=normalize(mat3(frame.view)*decodeWorldNormal(guide.gb));
                guided=true;
            }
        }
        if(!guided)
        {
            vec2 surfaceUV=halfSurfaceUV(q);
            sampleDepth=depthAt(surfaceUV);if(sampleDepth<=0.0)continue;
            sampleNormal=normalView(surfaceUV);
        }
        float weight=(x==0?1.0-fraction.x:fraction.x)*(y==0?1.0-fraction.y:fraction.y)
            *exp(-abs(depth-sampleDepth)/max(0.05,depth*0.02))*pow(max(dot(n,sampleNormal),0.0),16.0);
        if(frame.options.y>0.5)gi+=texture(sampler2D(sourceImage,linearSampler),q).rgb*weight;
        if(frame.options.z>0.5)reflection+=texture(sampler2D(reflectionImage,linearSampler),q)*weight;
        total+=weight;
    }
    if(total>1e-5) {gi/=total;reflection/=total;}
    vec3 environment=frame.options.z>0.5?texture(sampler2D(environmentImage,linearSampler),p).rgb:vec3(0);
    return vec4(max(hdr.rgb+gi+reflection.rgb-environment*reflection.a,vec3(0)),hdr.a);
}
void main()
{
#if SCREEN_SPACE_LIGHTING
    vec4 gi,reflection;indirectLighting(uv,gi,reflection);
    result=operation.w>0.5?reflection:gi;
#else
    if(operation.x<0.5)result=hemisphereAO(uv);
    else if(operation.x<1.5)result=bilateralAO(uv);
    else result=composite(uv);
#endif
}
#endif
