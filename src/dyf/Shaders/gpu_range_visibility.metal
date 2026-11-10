#include <metal_stdlib>
using namespace metal;
#ifndef GPU_RANGE_OCCLUSION
#define GPU_RANGE_OCCLUSION 0
#endif
struct Range {float4 minimum,maximum;uint4 draw;};
struct Instance {float4x4 model,previousMVP;};
struct Frame {float4 planes[6];float4 depthRow,projection,lodScreenSizes;uint4 counts;};
bool finite4(float4 v) {return all(isfinite(v));}
#if GPU_RANGE_OCCLUSION
struct OcclusionFrame {float4x4 viewProjection;float4 viewport;uint4 info;};
bool occludedRange(float3 minimum,float3 maximum,constant OcclusionFrame& occlusion,texture2d<float> hizImage) {
    if(occlusion.info.x==0u)return false;
    float2 low=1e30f,high=-1e30f;float nearestDepth=1;
    for(uint corner=0;corner<8;++corner) {
        float3 position=select(minimum,maximum,bool3((corner&1u)!=0u,(corner&2u)!=0u,(corner&4u)!=0u));
        float4 clip=occlusion.viewProjection*float4(position,1);
        if(!finite4(clip) || clip.w<=1e-6f || clip.z<=0)return false;
        float3 ndc=clip.xyz/clip.w;float2 pixel=(ndc.xy*float2(.5f,-.5f)+.5f)*occlusion.viewport.xy;
        if(!finite4(float4(ndc,0)) || !finite4(float4(pixel,0,0)))return false;
        low=min(low,pixel);high=max(high,pixel);nearestDepth=min(nearestDepth,ndc.z);
    }
    if(high.x<0 || high.y<0 || low.x>=occlusion.viewport.x || low.y>=occlusion.viewport.y)return false;
    low=clamp(floor(low-occlusion.viewport.w),0.f,occlusion.viewport.xy-1);
    high=clamp(ceil(high+occlusion.viewport.w)-1,0.f,occlusion.viewport.xy-1);
    float2 extent=high-low+1;
    int level=clamp(int(ceil(log2(max(extent.x,extent.y))))-2,0,int(occlusion.info.x)-1);
    float footprint=float(4u<<uint(level));
    int2 size=int2(hizImage.get_width(uint(level)),hizImage.get_height(uint(level)));
    int2 first=clamp(int2(floor(low/footprint)),int2(0),size-1);
    int2 last=clamp(int2(floor(high/footprint)),int2(0),size-1);
    float blocker=0;
    for(int y=first.y;y<=last.y;++y)for(int x=first.x;x<=last.x;++x) {
        float depth=hizImage.read(uint2(x,y),uint(level)).r;
        if(!isfinite(depth) || depth<0 || depth>=1)return false;
        blocker=max(blocker,depth);
    }
    return blocker+occlusion.viewport.z<nearestDepth;
}
#endif
bool visibleRange(Range item,float4x4 model,constant Frame& frame
#if GPU_RANGE_OCCLUSION
    ,constant OcclusionFrame& occlusion,texture2d<float> hizImage
#endif
) {
#if !GPU_RANGE_OCCLUSION
    if(frame.counts.z==0)return true;
#endif
    if(!(item.minimum.w>0) || !finite4(item.minimum) || !finite4(item.maximum) ||
        any(item.minimum.xyz>item.maximum.xyz))return true;
    for(uint i=0;i<4;++i)if(!finite4(model[i]))return true;
    if(model[0].w!=0 || model[1].w!=0 || model[2].w!=0 || model[3].w!=1)return true;
    float3 center=(item.minimum.xyz+item.maximum.xyz)*.5,halfSize=(item.maximum.xyz-item.minimum.xyz)*.5;
    float3 localMagnitude=max(abs(item.minimum.xyz),abs(item.maximum.xyz));
    float3 worldCenter=(model*float4(center,1)).xyz;
    float3 extent=abs(model[0].xyz)*halfSize.x+abs(model[1].xyz)*halfSize.y+abs(model[2].xyz)*halfSize.z;
    float3 magnitude=abs(model[3].xyz)+abs(model[0].xyz)*localMagnitude.x+
        abs(model[1].xyz)*localMagnitude.y+abs(model[2].xyz)*localMagnitude.z;
    float3 low=worldCenter-extent,high=worldCenter+extent;
    if(!finite4(float4(low,0)) || !finite4(float4(high,0)) || any(magnitude>float3(1e21)))return true;
    if(frame.counts.z!=0)for(uint i=0;i<6;++i) {
        float4 plane=frame.planes[i];
        float distance=plane.w+dot(plane.xyz,select(low,high,plane.xyz>=float3(0)));
        float scale=abs(plane.w)+dot(abs(plane.xyz),magnitude);
        if(!finite4(plane) || !finite4(float4(distance,scale,0,0)) || scale>1e32)return true;
        // Same conservative transform-rounding margin as shadow range culling.
        if(distance < -1e-4*max(1.0,scale))return false;
    }
#if GPU_RANGE_OCCLUSION
    // Inflate transformed bounds before projection, then use the existing HiZ pixel/depth margins.
    float3 padding=1e-4f*max(float3(1),magnitude);
    if(occludedRange(low-padding,high+padding,occlusion,hizImage))return false;
#endif
    return true;
}
kernel void kernelMain(uint3 group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_threadgroup]],
    device const Range* ranges [[buffer(0)]],device const Instance* sourceInstances [[buffer(1)]],
    device const uint* sourceArguments [[buffer(2)]],device const uint2* sourceKeys [[buffer(3)]],
    device Instance* instances [[buffer(4)]],device atomic_uint* arguments [[buffer(5)]],
    device uint2* keys [[buffer(6)]],constant Frame& frame [[buffer(7)]],
#if GPU_RANGE_OCCLUSION
    texture2d<float> hizImage [[texture(8)]],constant OcclusionFrame& occlusion [[buffer(9)]],sampler hizSampler [[sampler(10)]],
#endif
    constant uint4& operation [[buffer(15)]]) {
    uint id=group.x+group.y*operation.y;if(id>=operation.x)return;
    Range item=ranges[id];uint src=item.draw.x*5,dst=id*5;
    if(lane==0) {
        atomic_store_explicit(arguments+dst,item.draw.z,memory_order_relaxed);
        atomic_store_explicit(arguments+dst+1,0,memory_order_relaxed);
        atomic_store_explicit(arguments+dst+2,sourceArguments[src+2]+item.draw.y,memory_order_relaxed);
        atomic_store_explicit(arguments+dst+3,sourceArguments[src+3],memory_order_relaxed);
        atomic_store_explicit(arguments+dst+4,item.draw.w,memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_device);
    uint count=sourceArguments[src+1],first=sourceArguments[src+4];
    for(uint i=lane;i<count;i+=64) {
        Instance value=sourceInstances[first+i];
        if(!visibleRange(item,value.model,frame
#if GPU_RANGE_OCCLUSION
            ,occlusion,hizImage
#endif
        ))continue;
        uint row=item.draw.w+atomic_fetch_add_explicit(arguments+dst+1,1,memory_order_relaxed);
        instances[row]=value;keys[row]=sourceKeys[first+i]+uint2(0,item.draw.y/3u);
    }
}
