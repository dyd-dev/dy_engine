#include <metal_stdlib>
using namespace metal;
struct Input {float4 minimum,maximum;uint4 ids;};
struct Batch {uint indexCount,firstIndex;int vertexOffset;uint instanceOffset;uint4 capacityPadding;};
struct View {float4x4 projection;float4 receiverPlanes[18];uint4 info;};
struct Instance {float4x4 model,previous;};
struct Operation {uint4 counts,settings;};
bool visibleToLight(Input item,float4x4 model,View view) {
    if(!(item.minimum.w>0) || !all(isfinite(item.minimum)) || !all(isfinite(item.maximum)) ||
        any(item.minimum.xyz>item.maximum.xyz))return true;
    for(uint i=0;i<4;++i)if(!all(isfinite(model[i])) || !all(isfinite(view.projection[i])))return true;
    if(model[0].w!=0 || model[1].w!=0 || model[2].w!=0 || model[3].w!=1)return true;
    float3 center=(item.minimum.xyz+item.maximum.xyz)*.5f,halfSize=(item.maximum.xyz-item.minimum.xyz)*.5f;
    float3 localMagnitude=max(abs(item.minimum.xyz),abs(item.maximum.xyz));
    float3 worldCenter=(model*float4(center,1)).xyz;
    float3 extent=abs(model[0].xyz)*halfSize.x+abs(model[1].xyz)*halfSize.y+abs(model[2].xyz)*halfSize.z;
    float3 magnitude=abs(model[3].xyz)+abs(model[0].xyz)*localMagnitude.x+
        abs(model[1].xyz)*localMagnitude.y+abs(model[2].xyz)*localMagnitude.z;
    float3 low=worldCenter-extent,high=worldCenter+extent;
    if(!all(isfinite(low)) || !all(isfinite(high)) || any(magnitude>1e21f))return true;
    const float4x4 rows=transpose(view.projection);
    for(uint i=0;i<6+view.info.x;++i) {
        float4 plane,scalePlane;
        if(i<6) {
            plane=i==4?rows[2]:rows[3]+(i%2==0?1.f:-1.f)*rows[i/2];
            scalePlane=i==4?abs(rows[2]):abs(rows[3])+abs(rows[i/2]);
            if(all(plane.xyz==0))return true;
        } else {plane=view.receiverPlanes[i-6];scalePlane=abs(plane);}
        float distance=plane.w+dot(plane.xyz,select(low,high,plane.xyz>=0));
        float scale=scalePlane.w+dot(scalePlane.xyz,magnitude);
        if(!all(isfinite(plane)) || !isfinite(distance) || !isfinite(scale) || scale>1e32f)return true;
        // Cover f32 plane conversion, transformed-bound rounding and fused operations.
        if(distance < -1e-4f*max(1.f,scale))return false;
    }
    return true;
}
kernel void kernelMain(uint3 threadId [[thread_position_in_grid]],device const Input* inputs [[buffer(0)]],
    device const Batch* batches [[buffer(1)]],device const View* views [[buffer(2)]],
    device const Instance* models [[buffer(3)]],device Instance* visible [[buffer(4)]],
    device atomic_uint* arguments [[buffer(5)]],constant Operation& operation [[buffer(15)]]) {
    const uint4 counts=operation.counts,settings=operation.settings;
    uint id=threadId.x+threadId.y*counts.y;
    if(counts.x==0) {
        if(id>=counts.w*settings.x)return;
        uint view=id/counts.w,b=id%counts.w;Batch batch=batches[b];uint offset=id*5;
        atomic_store_explicit(arguments+offset,batch.indexCount,memory_order_relaxed);
        atomic_store_explicit(arguments+offset+1,0,memory_order_relaxed);
        atomic_store_explicit(arguments+offset+2,batch.firstIndex,memory_order_relaxed);
        atomic_store_explicit(arguments+offset+3,uint(batch.vertexOffset),memory_order_relaxed);
        atomic_store_explicit(arguments+offset+4,view*settings.y+batch.instanceOffset,memory_order_relaxed);
    } else {
        if(id>=counts.z*settings.x)return;
        uint view=id/counts.z;if(views[view].info.y==0)return;
        Input item=inputs[id%counts.z];Instance instance=models[item.ids.x];
        if(!visibleToLight(item,instance.model,views[view]))return;
        Batch batch=batches[item.ids.y];uint offset=(view*counts.w+item.ids.y)*5;
        uint slot=atomic_fetch_add_explicit(arguments+offset+1,1,memory_order_relaxed);
        if(slot>=batch.capacityPadding.x) {atomic_fetch_min_explicit(arguments+offset+1,batch.capacityPadding.x,memory_order_relaxed);return;}
        visible[view*settings.y+batch.instanceOffset+slot]=instance;
    }
}
