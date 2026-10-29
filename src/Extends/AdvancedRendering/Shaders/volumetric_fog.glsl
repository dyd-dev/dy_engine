#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
layout(set=0,binding=0) uniform texture2D sceneImage;
layout(set=0,binding=1) uniform texture2D depthImage;
layout(set=0,binding=2) uniform texture2D fogImage;
layout(set=0,binding=3) uniform texture2D shadowImage;
layout(set=0,binding=8) uniform sampler pointSampler;
layout(set=0,binding=12) uniform Frame {
    mat4 inverseViewProjection,view;
    vec4 camera,size,fog,scattering;
    uvec4 options;
} frame;
struct Directional {vec4 directionIntensity,color;};
struct Point {vec4 positionRange,colorIntensity;};
struct Spot {vec4 positionRange,directionOuterCos,colorIntensity,cone;};
struct Area {vec4 positionIntensity,direction,up,color;};
layout(set=0,binding=13) uniform Lights {
    vec4 cameraPosition,direction,color,ambient,shadow,pbr,environment,pointPosition,pointColor,counts,areaCounts;
    Directional directional[4];Point point[16];Spot spot[16];Area rect[4],disc[4];vec4 shadowLight;
} lights;
layout(set=0,binding=16) uniform ShadowMatrix {
    mat4 lightViewProjectionMatrix[128];vec4 atlasRect[128];
    vec4 directionalViews[4],pointViews[16],spotViews[16],directionalSplits[4];
    mat4 cameraView;vec4 filterParams;
    vec4 viewProjectionParams[128];vec4 softShadowParams;
} shadows;
layout(push_constant) uniform Operation {vec4 values;} operation;
const float PI=3.14159265359;
float Phase(float cosine)
{
    float g=frame.scattering.w;
    return (1-g*g)/(4*PI*pow(max(1+g*g-2*g*cosine,1e-6),1.5));
}
float Falloff(float distance,float range)
{
    if(range<=0 || distance>=range) return 0;
    float x=distance/range;
    return pow(max(1-x*x*x*x,0),2)/max(distance*distance,.0001);
}
#define DY_SHADOW_PROJECTION(index) shadows.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadows.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadows.atlasRect[index]
#define DY_SHADOW_DIMENSIONS textureSize(sampler2D(shadowImage,pointSampler),0)
#define DY_SHADOW_READ(pixel) texelFetch(sampler2D(shadowImage,pointSampler),pixel,0).r
#define DY_SHADOW_FILTER shadows.filterParams
#define DY_SHADOW_SOFT shadows.softShadowParams
#include "shadow_filter.glsl"

float ShadowView(int index,vec3 position,float strength,float sourceRadius,int pointBase,vec3 toLight)
{
    if(index<0 || index>=128) return 1;
    vec4 clip=shadows.lightViewProjectionMatrix[index]*vec4(position,1);
    if(clip.w<=0) return 1;
    vec3 ndc=clip.xyz/clip.w;
    vec2 coordinate=ndc.xy*vec2(.5,-.5)+.5;
    if(any(lessThan(coordinate,vec2(0))) || any(greaterThan(coordinate,vec2(1))) || ndc.z<0 || ndc.z>1) return 1;
    vec4 tile=shadows.atlasRect[index];
    ivec2 dimensions=textureSize(sampler2D(shadowImage,pointSampler),0);
    ivec2 minimum=ivec2(ceil(tile.xy*vec2(dimensions)));
    ivec2 maximum=ivec2(floor((tile.xy+tile.zw)*vec2(dimensions)))-1;
    if(any(lessThan(maximum,minimum))) return 1;
    vec2 pixel=(tile.xy+coordinate*tile.zw)*vec2(dimensions)-.5;
    ivec2 origin=ivec2(floor(pixel));vec2 fraction=fract(pixel);
    float visibility=0,receiver=ndc.z-max(lights.shadow.x,0);
    if(sourceRadius>0 && shadows.softShadowParams.x>0)
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,coordinate,receiver,clip.w,toLight,sourceRadius));
    for(int y=0;y<2;++y) for(int x=0;x<2;++x)
    {
        ivec2 tap=clamp(origin+ivec2(x,y),minimum,maximum);
        float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);
        visibility+=(receiver<=texelFetch(sampler2D(shadowImage,pointSampler),tap,0).r?1:0)*weight;
    }
    return 1-clamp(strength,0,1)*(1-visibility);
}
float Visibility(int type,int lightIndex,vec3 position,vec3 toLight)
{
    vec4 views=type==1?shadows.directionalViews[lightIndex]:(type==2?shadows.pointViews[lightIndex]:shadows.spotViews[lightIndex]);
    int count=int(views.y+.5),base=int(views.x+.5);
    if(count<=0 || base<0 || base+count>128 || views.z<=0) return 1;
    int local=0;float depth=0;
    if(type==1)
    {
        depth=-(shadows.cameraView*vec4(position,1)).z;
        float lastSplit=shadows.directionalSplits[lightIndex][min(count,4)-1];
        if(lastSplit>0 && depth>lastSplit) return 1;
        while(local<min(count,4)-1 && depth>shadows.directionalSplits[lightIndex][local]) ++local;
    }
    if(type==2)
    {
        vec3 direction=-toLight,magnitude=abs(direction);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z) local=direction.x>=0?0:1;
        else if(magnitude.y>=magnitude.z) local=direction.y>=0?2:3;
        else local=direction.z>=0?4:5;
        if(local>=count) return 1;
    }
    float visibility=ShadowView(base+local,position,views.z,views.w,type==2?base:-1,toLight);
    if(type==1 && local<min(count,4)-1)
    {
        float previous=local==0?shadows.filterParams.w:shadows.directionalSplits[lightIndex][local-1];
        float split=shadows.directionalSplits[lightIndex][local];
        float blend=smoothstep(mix(previous,split,.9),split,depth);
        if(blend>0) visibility=mix(visibility,ShadowView(base+local+1,position,views.z,views.w,type==2?base:-1,toLight),blend);
    }
    return visibility;
}
struct DirectionalRayCache {vec3 direction[4];vec3 radiance[4];uint mask;};
vec3 Incident(vec3 position,vec3 ray,DirectionalRayCache directional)
{
    vec3 radiance=vec3(0);
    if(lights.counts.w<.5) return radiance;
    for(int i=0;i<4;++i)
    {
        if((directional.mask&(1u<<uint(i)))==0u)continue;
        radiance+=directional.radiance[i]*Visibility(1,i,position,directional.direction[i]);
    }
    for(int i=0;i<clamp(int(lights.counts.y+.5),0,16);++i)
    {
        Point light=lights.point[i];vec3 delta=light.positionRange.xyz-position;float distance=length(delta);
        if(distance<=1e-4 || distance>=light.positionRange.w) continue;
        vec3 l=delta/distance;
        radiance+=max(light.colorIntensity.rgb,vec3(0))*max(light.colorIntensity.w,0)*Falloff(distance,light.positionRange.w)*
            Phase(dot(ray,l))*Visibility(2,i,position,l);
    }
    for(int i=0;i<clamp(int(lights.counts.z+.5),0,16);++i)
    {
        Spot light=lights.spot[i];vec3 delta=light.positionRange.xyz-position;float distance=length(delta);
        if(distance<=1e-4 || distance>=light.positionRange.w || dot(light.directionOuterCos.xyz,light.directionOuterCos.xyz)<1e-12) continue;
        vec3 l=delta/distance;
        float cone=smoothstep(light.directionOuterCos.w,max(light.cone.x,light.directionOuterCos.w+1e-4),dot(-l,normalize(light.directionOuterCos.xyz)));
        if(cone>0) radiance+=max(light.colorIntensity.rgb,vec3(0))*max(light.colorIntensity.w,0)*Falloff(distance,light.positionRange.w)*cone*
            Phase(dot(ray,l))*Visibility(3,i,position,l);
    }
    return radiance;
}
vec3 World(vec2 coordinate,float depth)
{
    vec4 world=frame.inverseViewProjection*vec4(coordinate*vec2(2,-2)+vec2(-1,1),depth,1);
    return world.xyz/world.w;
}
vec4 Integrate(vec2 coordinate,float depth)
{
    vec3 nearPoint=World(coordinate,0),origin=frame.options.y!=0u?frame.camera.xyz:nearPoint;
    vec4 farPoint=frame.inverseViewProjection*vec4(coordinate*vec2(2,-2)+vec2(-1,1),1,1);
    vec3 ray=normalize(abs(farPoint.w)>1e-8?farPoint.xyz/farPoint.w-origin:farPoint.xyz);
    float distance=depth<1?max(dot(World(coordinate,depth)-origin,ray),0):frame.fog.w;
    distance=min(distance,frame.fog.w);
    float dt=distance/float(frame.options.x),transmission=1;
    vec3 scattering=vec3(0);
    // Four ray-local entries cache direction/phase; shadow visibility stays per sample.
    DirectionalRayCache directional;directional.mask=0u;
    int directionalCount=frame.options.x>0u && frame.fog.x>0 && !(lights.counts.w<.5)?
        clamp(int(lights.counts.x+.5),0,4):0;
    for(int lightIndex=0;lightIndex<4;++lightIndex)
    {
        directional.direction[lightIndex]=vec3(0);directional.radiance[lightIndex]=vec3(0);
        if(lightIndex>=directionalCount)continue;
        vec3 direction=lights.directional[lightIndex].directionIntensity.xyz;
        if(dot(direction,direction)<1e-12)continue;
        vec3 l=normalize(direction);directional.direction[lightIndex]=l;
        directional.radiance[lightIndex]=max(lights.directional[lightIndex].color.rgb,vec3(0))*
            max(lights.directional[lightIndex].directionIntensity.w,0)*Phase(dot(ray,l));
        directional.mask|=1u<<uint(lightIndex);
    }
    // 13 예제의 중점 적분과 Beer–Lambert 투과율을 실제 깊이·광원으로 평가한다.
    for(uint i=0;i<frame.options.x && frame.fog.x>0;++i)
    {
        vec3 position=origin+ray*((float(i)+.5)*dt);
        float density=frame.fog.x*exp(-max(position.z-frame.fog.z,0)*frame.fog.y);
        float stepTransmission=exp(-density*dt);
        scattering+=transmission*(1-stepTransmission)*frame.scattering.rgb*Incident(position,ray,directional);
        transmission*=stepTransmission;
    }
    return vec4(clamp(scattering,vec3(0),vec3(65504)),clamp(transmission,0,1));
}
ivec2 SourcePixel(vec2 coordinate)
{
    return clamp(ivec2(coordinate*frame.size.xy),ivec2(0),ivec2(frame.size.xy)-1);
}
float Guide(ivec2 pixel,float depth)
{
    if(depth>=1) return frame.fog.w;
    return max(-(frame.view*vec4(World((vec2(pixel)+.5)*frame.size.zw,depth),1)).z,0);
}
void main()
{
    ivec2 source=SourcePixel(uv);
    float depth=texelFetch(sampler2D(depthImage,pointSampler),source,0).r;
    vec2 coordinate=(vec2(source)+.5)*frame.size.zw;
    if(operation.values.x<.5)
    {
        outputColor=Integrate(coordinate,depth);
        return;
    }
    ivec2 dimensions=textureSize(sampler2D(fogImage,pointSampler),0);
    vec2 pixel=uv*vec2(dimensions)-.5,fraction=fract(pixel);ivec2 origin=ivec2(floor(pixel));
    float guide=Guide(source,depth),total=0;vec4 fog=vec4(0);
    for(int y=0;y<2;++y) for(int x=0;x<2;++x)
    {
        ivec2 tap=clamp(origin+ivec2(x,y),ivec2(0),dimensions-1);
        ivec2 full=SourcePixel((vec2(tap)+.5)/vec2(dimensions));
        float tapDepth=texelFetch(sampler2D(depthImage,pointSampler),full,0).r;
        float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);
        if((depth>=1)!=(tapDepth>=1)) weight=0;
        weight*=exp(-abs(Guide(full,tapDepth)-guide)/max(.01,guide*.02));
        fog+=texelFetch(sampler2D(fogImage,pointSampler),tap,0)*weight;total+=weight;
    }
    // 얇은 전경을 놓친 저해상도 표본은 해당 픽셀을 직접 적분해 깊이 번짐을 막는다.
    fog=total>1e-5?fog/total:Integrate(coordinate,depth);
    vec3 hdr=max(texelFetch(sampler2D(sceneImage,pointSampler),source,0).rgb,vec3(0));
    outputColor=vec4(clamp(hdr*fog.a+fog.rgb,vec3(0),vec3(65504)),1);
}
