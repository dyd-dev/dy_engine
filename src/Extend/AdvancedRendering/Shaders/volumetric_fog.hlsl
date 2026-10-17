Texture2D<float4> sceneImage:register(t0,space0);
Texture2D<float> depthImage:register(t1,space0);
Texture2D<float4> fogImage:register(t2,space0);
Texture2D<float> shadowImage:register(t3,space0);
SamplerState pointSampler:register(s8,space0);
struct FrameGPU
{
    column_major float4x4 inverseViewProjection,view;
    float4 camera,size,fog,scattering;
    uint4 options;
};
cbuffer Frame:register(b12,space0) {FrameGPU frame;};
struct Directional {float4 directionIntensity,color;};
struct PointLight {float4 positionRange,colorIntensity;};
struct Spot {float4 positionRange,directionOuterCos,colorIntensity,cone;};
struct Area {float4 positionIntensity,direction,up,color;};
struct LightData
{
    float4 cameraPosition,direction,color,ambient,shadow,pbr,environment,pointPosition,pointColor,counts,areaCounts;
    Directional directional[4];PointLight pointLights[16];Spot spot[16];Area rect[4],disc[4];float4 shadowLight;
};
cbuffer Lights:register(b13,space0) {LightData lights;};
struct ShadowData
{
    column_major float4x4 lightViewProjectionMatrix[128];float4 atlasRect[128];
    float4 directionalViews[4],pointViews[16],spotViews[16],directionalSplits[4];
    column_major float4x4 cameraView;float4 filterParams;
    float4 viewProjectionParams[128];float4 softShadowParams;
};
cbuffer ShadowMatrix:register(b16,space0) {ShadowData shadows;};
cbuffer Operation:register(b15,space0) {float4 operation;};
static const float PI=3.14159265359;
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
int2 DyShadowDimensions()
{
    uint width,height;shadowImage.GetDimensions(width,height);return int2(width,height);
}
#define DY_SHADOW_PROJECTION(index) shadows.viewProjectionParams[index]
#define DY_SHADOW_MATRIX(index) shadows.lightViewProjectionMatrix[index]
#define DY_SHADOW_TILE(index) shadows.atlasRect[index]
#define DY_SHADOW_DIMENSIONS DyShadowDimensions()
#define DY_SHADOW_READ(pixel) shadowImage.Load(int3(pixel,0))
#define DY_SHADOW_FILTER shadows.filterParams
#define DY_SHADOW_SOFT shadows.softShadowParams
#include "shadow_filter.hlsl"

float ShadowView(int index,float3 position,float strength,float sourceRadius,int pointBase,float3 toLight)
{
    if(index<0 || index>=128) return 1;
    float4 clip=mul(shadows.lightViewProjectionMatrix[index],float4(position,1));
    if(clip.w<=0) return 1;
    float3 ndc=clip.xyz/clip.w;
    float2 coordinate=ndc.xy*float2(.5,-.5)+.5;
    if(any(coordinate<0) || any(coordinate>1) || ndc.z<0 || ndc.z>1) return 1;
    float4 tile=shadows.atlasRect[index];
    uint width,height;shadowImage.GetDimensions(width,height);int2 dimensions=int2(width,height);
    int2 minimum=int2(ceil(tile.xy*dimensions));
    int2 maximum=int2(floor((tile.xy+tile.zw)*dimensions))-1;
    if(any(maximum<minimum)) return 1;
    float2 pixel=(tile.xy+coordinate*tile.zw)*dimensions-.5;
    int2 origin=int2(floor(pixel));float2 fraction=frac(pixel);
    float visibility=0,receiver=ndc.z-max(lights.shadow.x,0);
    if(sourceRadius>0 && shadows.softShadowParams.x>0)
        return 1-clamp(strength,0.0,1.0)*(1-DyShadowPcss(index,pointBase,coordinate,receiver,clip.w,toLight,sourceRadius));
    for(int y=0;y<2;++y) for(int x=0;x<2;++x)
    {
        int2 tap=clamp(origin+int2(x,y),minimum,maximum);
        float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);
        visibility+=(receiver<=shadowImage.Load(int3(tap,0))?1:0)*weight;
    }
    return 1-saturate(strength)*(1-visibility);
}
float Visibility(int type,int lightIndex,float3 position,float3 toLight)
{
    float4 views=type==1?shadows.directionalViews[lightIndex]:(type==2?shadows.pointViews[lightIndex]:shadows.spotViews[lightIndex]);
    int count=int(views.y+.5),base=int(views.x+.5);
    if(count<=0 || base<0 || base+count>128 || views.z<=0) return 1;
    int local=0;float depth=0;
    if(type==1)
    {
        depth=-mul(shadows.cameraView,float4(position,1)).z;
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
    float visibility=ShadowView(base+local,position,views.z,views.w,type==2?base:-1,toLight);
    if(type==1 && local<min(count,4)-1)
    {
        float previous=local==0?shadows.filterParams.w:shadows.directionalSplits[lightIndex][local-1];
        float split=shadows.directionalSplits[lightIndex][local];
        float blend=smoothstep(lerp(previous,split,.9),split,depth);
        if(blend>0) visibility=lerp(visibility,ShadowView(base+local+1,position,views.z,views.w,type==2?base:-1,toLight),blend);
    }
    return visibility;
}
float3 Incident(float3 position,float3 ray)
{
    float3 radiance=0;
    if(lights.counts.w<.5) return radiance;
    for(int i=0;i<clamp(int(lights.counts.x+.5),0,4);++i)
    {
        float3 direction=lights.directional[i].directionIntensity.xyz;
        if(dot(direction,direction)<1e-12) continue;
        float3 l=normalize(direction);
        radiance+=max(lights.directional[i].color.rgb,0)*max(lights.directional[i].directionIntensity.w,0)*
            Phase(dot(ray,l))*Visibility(1,i,position,l);
    }
    for(int i=0;i<clamp(int(lights.counts.y+.5),0,16);++i)
    {
        PointLight light=lights.pointLights[i];float3 delta=light.positionRange.xyz-position;float distance=length(delta);
        if(distance<=1e-4 || distance>=light.positionRange.w) continue;
        float3 l=delta/distance;
        radiance+=max(light.colorIntensity.rgb,0)*max(light.colorIntensity.w,0)*Falloff(distance,light.positionRange.w)*
            Phase(dot(ray,l))*Visibility(2,i,position,l);
    }
    for(int i=0;i<clamp(int(lights.counts.z+.5),0,16);++i)
    {
        Spot light=lights.spot[i];float3 delta=light.positionRange.xyz-position;float distance=length(delta);
        if(distance<=1e-4 || distance>=light.positionRange.w || dot(light.directionOuterCos.xyz,light.directionOuterCos.xyz)<1e-12) continue;
        float3 l=delta/distance;
        float cone=smoothstep(light.directionOuterCos.w,max(light.cone.x,light.directionOuterCos.w+1e-4),dot(-l,normalize(light.directionOuterCos.xyz)));
        if(cone>0) radiance+=max(light.colorIntensity.rgb,0)*max(light.colorIntensity.w,0)*Falloff(distance,light.positionRange.w)*cone*
            Phase(dot(ray,l))*Visibility(3,i,position,l);
    }
    return radiance;
}
float3 World(float2 coordinate,float depth)
{
    float4 world=mul(frame.inverseViewProjection,float4(coordinate*float2(2,-2)+float2(-1,1),depth,1));
    return world.xyz/world.w;
}
float4 Integrate(float2 coordinate,float depth)
{
    float3 nearPoint=World(coordinate,0),origin=frame.options.y!=0?frame.camera.xyz:nearPoint;
    float4 farPoint=mul(frame.inverseViewProjection,float4(coordinate*float2(2,-2)+float2(-1,1),1,1));
    float3 ray=normalize(abs(farPoint.w)>1e-8?farPoint.xyz/farPoint.w-origin:farPoint.xyz);
    float distance=depth<1?max(dot(World(coordinate,depth)-origin,ray),0):frame.fog.w;
    distance=min(distance,frame.fog.w);
    float dt=distance/float(frame.options.x),transmission=1;
    float3 scattering=0;
    for(uint i=0;i<frame.options.x && frame.fog.x>0;++i)
    {
        float3 position=origin+ray*((float(i)+.5)*dt);
        float density=frame.fog.x*exp(-max(position.z-frame.fog.z,0)*frame.fog.y);
        float stepTransmission=exp(-density*dt);
        scattering+=transmission*(1-stepTransmission)*frame.scattering.rgb*Incident(position,ray);
        transmission*=stepTransmission;
    }
    return float4(clamp(scattering,0,65504),saturate(transmission));
}
int2 SourcePixel(float2 coordinate)
{
    return clamp(int2(coordinate*frame.size.xy),int2(0,0),int2(frame.size.xy)-1);
}
float Guide(int2 pixel,float depth)
{
    if(depth>=1) return frame.fog.w;
    return max(-mul(frame.view,float4(World((float2(pixel)+.5)*frame.size.zw,depth),1)).z,0);
}
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target
{
    int2 source=SourcePixel(uv);
    float depth=depthImage.Load(int3(source,0));
    float2 coordinate=(float2(source)+.5)*frame.size.zw;
    if(operation.x<.5) return Integrate(coordinate,depth);
    uint width,height;fogImage.GetDimensions(width,height);int2 dimensions=int2(width,height);
    float2 pixel=uv*dimensions-.5,fraction=frac(pixel);int2 origin=int2(floor(pixel));
    float guide=Guide(source,depth),total=0;float4 fog=0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x)
    {
        int2 tap=clamp(origin+int2(x,y),int2(0,0),dimensions-1);
        int2 full=SourcePixel((float2(tap)+.5)/dimensions);
        float tapDepth=depthImage.Load(int3(full,0));
        float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);
        if((depth>=1)!=(tapDepth>=1)) weight=0;
        weight*=exp(-abs(Guide(full,tapDepth)-guide)/max(.01,guide*.02));
        fog+=fogImage.Load(int3(tap,0))*weight;total+=weight;
    }
    fog=total>1e-5?fog/total:Integrate(coordinate,depth);
    float3 hdr=max(sceneImage.Load(int3(source,0)).rgb,0);
    return float4(clamp(hdr*fog.a+fog.rgb,0,65504),1);
}
