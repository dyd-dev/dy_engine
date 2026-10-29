#include <metal_stdlib>
using namespace metal;
struct Input {float4 position [[position]];float2 uv [[user(locn0)]];};
struct Frame
{
    float4x4 inverseViewProjection,view;
    float4 camera,size,fog,scattering;
    uint4 options;
};
struct Directional {float4 directionIntensity,color;};
struct Point {float4 positionRange,colorIntensity;};
struct Spot {float4 positionRange,directionOuterCos,colorIntensity,cone;};
struct Area {float4 positionIntensity,direction,up,color;};
struct Lights
{
    float4 cameraPosition,direction,color,ambient,shadow,pbr,environment,pointPosition,pointColor,counts,areaCounts;
    Directional directional[4];Point point[16];Spot spot[16];Area rect[4],disc[4];float4 shadowLight;
};
struct Shadows
{
    float4x4 lightViewProjectionMatrix[128];float4 atlasRect[128];
    float4 directionalViews[4],pointViews[16],spotViews[16],directionalSplits[4];
    float4x4 cameraView;float4 filterParams;
    float4 viewProjectionParams[128];float4 softShadowParams;
};
constant float PI=3.14159265359f;
float Phase(float cosine,constant Frame& frame)
{
    float g=frame.scattering.w;
    return (1-g*g)/(4*PI*pow(max(1+g*g-2*g*cosine,1e-6f),1.5f));
}
float Falloff(float distance,float range)
{
    if(range<=0 || distance>=range) return 0;
    float x=distance/range;
    return pow(max(1-x*x*x*x,0.f),2.f)/max(distance*distance,.0001f);
}
#define DY_SHADOW_STRUCT Shadows
#define DY_SHADOW_TEXTURE texture2d<float>
#define DY_SHADOW_PROJECTION(index) shadowData.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadowData.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadowData.atlasRect[index]
#define DY_SHADOW_DIMENSIONS int2(shadowImage.get_width(),shadowImage.get_height())
#define DY_SHADOW_READ(pixel) shadowImage.read(uint2(pixel)).r
#define DY_SHADOW_FILTER shadowData.filterParams
#define DY_SHADOW_SOFT shadowData.softShadowParams
#include "shadow_filter.metal"

float ShadowView(int index,float3 position,float strength,float sourceRadius,int pointBase,float3 toLight,texture2d<float> shadowImage,
    constant Lights& lights,constant Shadows& shadows)
{
    if(index<0 || index>=128) return 1;
    float4 clip=shadows.lightViewProjectionMatrix[index]*float4(position,1);
    if(clip.w<=0) return 1;
    float3 ndc=clip.xyz/clip.w;
    float2 coordinate=ndc.xy*float2(.5,-.5)+.5;
    if(any(coordinate<0) || any(coordinate>1) || ndc.z<0 || ndc.z>1) return 1;
    float4 tile=shadows.atlasRect[index];
    int2 dimensions=int2(shadowImage.get_width(),shadowImage.get_height());
    int2 minimum=int2(ceil(tile.xy*float2(dimensions)));
    int2 maximum=int2(floor((tile.xy+tile.zw)*float2(dimensions)))-1;
    if(any(maximum<minimum)) return 1;
    float2 pixel=(tile.xy+coordinate*tile.zw)*float2(dimensions)-.5;
    int2 origin=int2(floor(pixel));float2 fraction=fract(pixel);
    float visibility=0,receiver=ndc.z-max(lights.shadow.x,0.f);
    if(sourceRadius>0 && shadows.softShadowParams.x>0)
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,coordinate,receiver,clip.w,toLight,sourceRadius,shadows,shadowImage));
    for(int y=0;y<2;++y) for(int x=0;x<2;++x)
    {
        int2 tap=clamp(origin+int2(x,y),minimum,maximum);
        float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);
        visibility+=(receiver<=shadowImage.read(uint2(tap)).r?1:0)*weight;
    }
    return 1-clamp(strength,0.f,1.f)*(1-visibility);
}
float Visibility(int type,int lightIndex,float3 position,float3 toLight,texture2d<float> shadowImage,
    constant Lights& lights,constant Shadows& shadows)
{
    float4 views=type==1?shadows.directionalViews[lightIndex]:(type==2?shadows.pointViews[lightIndex]:shadows.spotViews[lightIndex]);
    int count=int(views.y+.5),base=int(views.x+.5);
    if(count<=0 || base<0 || base+count>128 || views.z<=0) return 1;
    int local=0;float depth=0;
    if(type==1)
    {
        depth=-(shadows.cameraView*float4(position,1)).z;
        float lastSplit=shadows.directionalSplits[lightIndex][min(count,4)-1];
        if(lastSplit>0 && depth>lastSplit) return 1;
        while(local<min(count,4)-1 && depth>shadows.directionalSplits[lightIndex][local]) ++local;
    }
    if(type==2)
    {
        float3 direction=-toLight,magnitude=abs(direction);
        if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z) local=direction.x>=0?0:1;
        else if(magnitude.y>=magnitude.z) local=direction.y>=0?2:3;
        else local=direction.z>=0?4:5;
        if(local>=count) return 1;
    }
    float visibility=ShadowView(base+local,position,views.z,views.w,type==2?base:-1,toLight,shadowImage,lights,shadows);
    if(type==1 && local<min(count,4)-1)
    {
        float previous=local==0?shadows.filterParams.w:shadows.directionalSplits[lightIndex][local-1];
        float split=shadows.directionalSplits[lightIndex][local];
        float blend=smoothstep(mix(previous,split,.9f),split,depth);
        if(blend>0) visibility=mix(visibility,ShadowView(base+local+1,position,views.z,views.w,type==2?base:-1,toLight,shadowImage,lights,shadows),blend);
    }
    return visibility;
}
struct DirectionalRayCache {float3 direction[4];float3 radiance[4];uint mask;};
float3 Incident(float3 position,float3 ray,thread const DirectionalRayCache& directional,texture2d<float> shadowImage,
    constant Frame& frame,constant Lights& lights,constant Shadows& shadows)
{
    float3 radiance=0;
    if(lights.counts.w<.5f) return radiance;
    for(int i=0;i<4;++i)
    {
        if((directional.mask&(1u<<uint(i)))==0u)continue;
        radiance+=directional.radiance[i]*Visibility(1,i,position,directional.direction[i],shadowImage,lights,shadows);
    }
    for(int i=0;i<clamp(int(lights.counts.y+.5),0,16);++i)
    {
        Point light=lights.point[i];float3 delta=light.positionRange.xyz-position;float distance=length(delta);
        if(distance<=1e-4f || distance>=light.positionRange.w) continue;
        float3 l=delta/distance;
        radiance+=max(light.colorIntensity.rgb,0.f)*max(light.colorIntensity.w,0.f)*Falloff(distance,light.positionRange.w)*
            Phase(dot(ray,l),frame)*Visibility(2,i,position,l,shadowImage,lights,shadows);
    }
    for(int i=0;i<clamp(int(lights.counts.z+.5),0,16);++i)
    {
        Spot light=lights.spot[i];float3 delta=light.positionRange.xyz-position;float distance=length(delta);
        if(distance<=1e-4f || distance>=light.positionRange.w || dot(light.directionOuterCos.xyz,light.directionOuterCos.xyz)<1e-12f) continue;
        float3 l=delta/distance;
        float cone=smoothstep(light.directionOuterCos.w,max(light.cone.x,light.directionOuterCos.w+1e-4f),dot(-l,normalize(light.directionOuterCos.xyz)));
        if(cone>0) radiance+=max(light.colorIntensity.rgb,0.f)*max(light.colorIntensity.w,0.f)*Falloff(distance,light.positionRange.w)*cone*
            Phase(dot(ray,l),frame)*Visibility(3,i,position,l,shadowImage,lights,shadows);
    }
    return radiance;
}
float3 World(float2 coordinate,float depth,constant Frame& frame)
{
    float4 world=frame.inverseViewProjection*float4(coordinate*float2(2,-2)+float2(-1,1),depth,1);
    return world.xyz/world.w;
}
float4 Integrate(float2 coordinate,float depth,texture2d<float> shadowImage,
    constant Frame& frame,constant Lights& lights,constant Shadows& shadows)
{
    float3 nearPoint=World(coordinate,0,frame),origin=frame.options.y!=0?frame.camera.xyz:nearPoint;
    float4 farPoint=frame.inverseViewProjection*float4(coordinate*float2(2,-2)+float2(-1,1),1,1);
    float3 ray=normalize(abs(farPoint.w)>1e-8f?farPoint.xyz/farPoint.w-origin:farPoint.xyz);
    float distance=depth<1?max(dot(World(coordinate,depth,frame)-origin,ray),0.f):frame.fog.w;
    distance=min(distance,frame.fog.w);
    float dt=distance/float(frame.options.x),transmission=1;
    float3 scattering=0;
    // Four ray-local entries cache direction/phase; shadow visibility stays per sample.
    DirectionalRayCache directional;directional.mask=0u;
    int directionalCount=frame.options.x>0 && frame.fog.x>0 && !(lights.counts.w<.5f)?
        clamp(int(lights.counts.x+.5),0,4):0;
    for(int lightIndex=0;lightIndex<4;++lightIndex)
    {
        directional.direction[lightIndex]=0;directional.radiance[lightIndex]=0;
        if(lightIndex>=directionalCount)continue;
        float3 direction=lights.directional[lightIndex].directionIntensity.xyz;
        if(dot(direction,direction)<1e-12f)continue;
        float3 l=normalize(direction);directional.direction[lightIndex]=l;
        directional.radiance[lightIndex]=max(lights.directional[lightIndex].color.rgb,0.f)*
            max(lights.directional[lightIndex].directionIntensity.w,0.f)*Phase(dot(ray,l),frame);
        directional.mask|=1u<<uint(lightIndex);
    }
    for(uint i=0;i<frame.options.x && frame.fog.x>0;++i)
    {
        float3 position=origin+ray*((float(i)+.5)*dt);
        float density=frame.fog.x*exp(-max(position.z-frame.fog.z,0.f)*frame.fog.y);
        float stepTransmission=exp(-density*dt);
        scattering+=transmission*(1-stepTransmission)*frame.scattering.rgb*Incident(position,ray,directional,shadowImage,frame,lights,shadows);
        transmission*=stepTransmission;
    }
    return float4(clamp(scattering,0.f,65504.f),clamp(transmission,0.f,1.f));
}
int2 SourcePixel(float2 coordinate,constant Frame& frame)
{
    return clamp(int2(coordinate*frame.size.xy),int2(0),int2(frame.size.xy)-1);
}
float Guide(int2 pixel,float depth,constant Frame& frame)
{
    if(depth>=1) return frame.fog.w;
    return max(-(frame.view*float4(World((float2(pixel)+.5)*frame.size.zw,depth,frame),1)).z,0.f);
}
fragment float4 fragmentMain(Input input [[stage_in]],
    texture2d<float> sceneImage [[texture(0)]],texture2d<float> depthImage [[texture(1)]],
    texture2d<float> fogImage [[texture(2)]],texture2d<float> shadowImage [[texture(3)]],
    sampler pointSampler [[sampler(8)]],constant Frame& frame [[buffer(12)]],
    constant Lights& lights [[buffer(13)]],constant Shadows& shadows [[buffer(16)]],constant float4& operation [[buffer(15)]])
{
    int2 source=SourcePixel(input.uv,frame);
    float depth=depthImage.read(uint2(source)).r;
    float2 coordinate=(float2(source)+.5)*frame.size.zw;
    if(operation.x<.5f) return Integrate(coordinate,depth,shadowImage,frame,lights,shadows);
    int2 dimensions=int2(fogImage.get_width(),fogImage.get_height());
    float2 pixel=input.uv*float2(dimensions)-.5,fraction=fract(pixel);int2 origin=int2(floor(pixel));
    float guide=Guide(source,depth,frame),total=0;float4 fog=0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x)
    {
        int2 tap=clamp(origin+int2(x,y),int2(0),dimensions-1);
        int2 full=SourcePixel((float2(tap)+.5)/float2(dimensions),frame);
        float tapDepth=depthImage.read(uint2(full)).r;
        float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);
        if((depth>=1)!=(tapDepth>=1)) weight=0;
        weight*=exp(-abs(Guide(full,tapDepth,frame)-guide)/max(.01f,guide*.02f));
        fog+=fogImage.read(uint2(tap))*weight;total+=weight;
    }
    fog=total>1e-5f?fog/total:Integrate(coordinate,depth,shadowImage,frame,lights,shadows);
    float3 hdr=max(sceneImage.read(uint2(source)).rgb,0.f);
    return float4(clamp(hdr*fog.a+fog.rgb,0.f,65504.f),1);
}
