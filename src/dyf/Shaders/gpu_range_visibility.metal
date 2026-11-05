#include <metal_stdlib>
using namespace metal;
struct Range {float4 minimum,maximum;uint4 draw;};
struct Instance {float4x4 model,previousMVP;};
struct Frame {float4 planes[6];float4 depthRow,projection,lodScreenSizes;uint4 counts;};
bool finite4(float4 v) {return all(isfinite(v));}
bool visibleRange(Range item,float4x4 model,constant Frame& frame) {
    if(frame.counts.z==0 || !(item.minimum.w>0) || !finite4(item.minimum) || !finite4(item.maximum) ||
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
    for(uint i=0;i<6;++i) {
        float4 plane=frame.planes[i];
        float distance=plane.w+dot(plane.xyz,select(low,high,plane.xyz>=float3(0)));
        float scale=abs(plane.w)+dot(abs(plane.xyz),magnitude);
        if(!finite4(plane) || !finite4(float4(distance,scale,0,0)) || scale>1e32)return true;
        // Same conservative transform-rounding margin as shadow range culling.
        if(distance < -1e-4*max(1.0,scale))return false;
    }
    return true;
}
kernel void kernelMain(uint3 group [[threadgroup_position_in_grid]],uint lane [[thread_index_in_threadgroup]],
    device const Range* ranges [[buffer(0)]],device const Instance* sourceInstances [[buffer(1)]],
    device const uint* sourceArguments [[buffer(2)]],device const uint2* sourceKeys [[buffer(3)]],
    device Instance* instances [[buffer(4)]],device atomic_uint* arguments [[buffer(5)]],
    device uint2* keys [[buffer(6)]],constant Frame& frame [[buffer(7)]],constant uint4& operation [[buffer(15)]]) {
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
        Instance value=sourceInstances[first+i];if(!visibleRange(item,value.model,frame))continue;
        uint row=item.draw.w+atomic_fetch_add_explicit(arguments+dst+1,1,memory_order_relaxed);
        instances[row]=value;keys[row]=sourceKeys[first+i]+uint2(0,item.draw.y/3u);
    }
}
