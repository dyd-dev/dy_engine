#ifndef GPU_RANGE_OCCLUSION
#define GPU_RANGE_OCCLUSION 0
#endif
struct Range {float4 minimum,maximum;uint4 draw;};
struct Instance {column_major float4x4 model,previousMVP;};
StructuredBuffer<Range> ranges:register(t0,space0);
StructuredBuffer<Instance> sourceInstances:register(t1,space0);
StructuredBuffer<uint> sourceArguments:register(t2,space0);
StructuredBuffer<uint2> sourceKeys:register(t3,space0);
RWStructuredBuffer<Instance> instances:register(u4,space0);
RWStructuredBuffer<uint> arguments:register(u5,space0);
RWStructuredBuffer<uint2> keys:register(u6,space0);
cbuffer Frame:register(b7,space0) {float4 planes[6];float4 depthRow,projection,lodScreenSizes;uint4 counts;};
cbuffer Operation:register(b15,space0) {uint4 operation;};
bool finite4(float4 v) {return all(isfinite(v));}
#if GPU_RANGE_OCCLUSION
Texture2D<float> hizImage:register(t8,space0);
cbuffer OcclusionFrame:register(b9,space0) {column_major float4x4 occlusionVP;float4 viewport;uint4 hizInfo;};
SamplerState hizSampler:register(s10,space0);
bool occludedRange(float3 minimum,float3 maximum) {
    if(hizInfo.x==0u)return false;
    float2 low=1e30,high=-1e30;float nearestDepth=1;
    for(uint corner=0;corner<8;++corner) {
        float3 position=float3((corner&1u)?maximum.x:minimum.x,(corner&2u)?maximum.y:minimum.y,(corner&4u)?maximum.z:minimum.z);
        float4 clip=mul(occlusionVP,float4(position,1));
        if(!finite4(clip) || clip.w<=1e-6 || clip.z<=0)return false;
        float3 ndc=clip.xyz/clip.w;float2 pixel=(ndc.xy*float2(.5,-.5)+.5)*viewport.xy;
        if(!finite4(float4(ndc,0)) || !finite4(float4(pixel,0,0)))return false;
        low=min(low,pixel);high=max(high,pixel);nearestDepth=min(nearestDepth,ndc.z);
    }
    if(high.x<0 || high.y<0 || low.x>=viewport.x || low.y>=viewport.y)return false;
    low=clamp(floor(low-viewport.w),0,viewport.xy-1);
    high=clamp(ceil(high+viewport.w)-1,0,viewport.xy-1);
    float2 extent=high-low+1;
    int level=clamp(int(ceil(log2(max(extent.x,extent.y))))-2,0,int(hizInfo.x)-1);
    float footprint=float(4u<<uint(level));
    uint width,height,levels;hizImage.GetDimensions(uint(level),width,height,levels);
    int2 first=clamp(int2(floor(low/footprint)),int2(0,0),int2(width,height)-1);
    int2 last=clamp(int2(floor(high/footprint)),int2(0,0),int2(width,height)-1);
    float blocker=0;
    for(int y=first.y;y<=last.y;++y)for(int x=first.x;x<=last.x;++x) {
        float depth=hizImage.Load(int3(x,y,level));
        if(!isfinite(depth) || depth<0 || depth>=1)return false;
        blocker=max(blocker,depth);
    }
    return blocker+viewport.z<nearestDepth;
}
#endif
bool visibleRange(Range item,column_major float4x4 model) {
#if !GPU_RANGE_OCCLUSION
    if(counts.z==0)return true;
#endif
    if(!(item.minimum.w>0) || !finite4(item.minimum) || !finite4(item.maximum) ||
        any(item.minimum.xyz>item.maximum.xyz))return true;
    float4x4 columns=transpose(model);
    for(uint i=0;i<4;++i)if(!finite4(model[i]))return true;
    if(columns[0].w!=0 || columns[1].w!=0 || columns[2].w!=0 || columns[3].w!=1)return true;
    float3 center=(item.minimum.xyz+item.maximum.xyz)*.5,halfSize=(item.maximum.xyz-item.minimum.xyz)*.5;
    float3 localMagnitude=max(abs(item.minimum.xyz),abs(item.maximum.xyz));
    float3 worldCenter=(mul(model,float4(center,1))).xyz;
    float3 extent=abs(columns[0].xyz)*halfSize.x+abs(columns[1].xyz)*halfSize.y+abs(columns[2].xyz)*halfSize.z;
    float3 magnitude=abs(columns[3].xyz)+abs(columns[0].xyz)*localMagnitude.x+
        abs(columns[1].xyz)*localMagnitude.y+abs(columns[2].xyz)*localMagnitude.z;
    float3 low=worldCenter-extent,high=worldCenter+extent;
    if(!finite4(float4(low,0)) || !finite4(float4(high,0)) || any(magnitude>1e21))return true;
    if(counts.z!=0)for(uint i=0;i<6;++i) {
        float4 plane=planes[i];
        float distance=plane.w+dot(plane.xyz,float3(plane.x>=0?high.x:low.x,plane.y>=0?high.y:low.y,plane.z>=0?high.z:low.z));
        float scale=abs(plane.w)+dot(abs(plane.xyz),magnitude);
        if(!finite4(plane) || !finite4(float4(distance,scale,0,0)) || scale>1e32)return true;
        // Same conservative transform-rounding margin as shadow range culling.
        if(distance < -1e-4*max(1.0,scale))return false;
    }
#if GPU_RANGE_OCCLUSION
    // Inflate transformed bounds before projection, then use the existing HiZ pixel/depth margins.
    float3 padding=1e-4*max(float3(1,1,1),magnitude);
    if(occludedRange(low-padding,high+padding))return false;
#endif
    return true;
}
[numthreads(64,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    uint id=group.x+group.y*operation.y;
    if(id>=operation.x)return;
    Range item=ranges[id];uint src=item.draw.x*5,dst=id*5;
    if(lane==0) {
        arguments[dst]=item.draw.z;arguments[dst+1]=0;
        arguments[dst+2]=sourceArguments[src+2]+item.draw.y;arguments[dst+3]=sourceArguments[src+3];
        arguments[dst+4]=item.draw.w;
    }
    DeviceMemoryBarrierWithGroupSync();
    uint count=sourceArguments[src+1],first=sourceArguments[src+4];
    for(uint i=lane;i<count;i+=64) {
        Instance value=sourceInstances[first+i];
        if(!visibleRange(item,value.model))continue;
        uint slot;InterlockedAdd(arguments[dst+1],1,slot);uint row=item.draw.w+slot;
        instances[row]=value;keys[row]=sourceKeys[first+i]+uint2(0,item.draw.y/3u);
    }
}
