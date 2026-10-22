#include <metal_stdlib>
using namespace metal;
#ifndef GPU_VISIBILITY_OCCLUSION
#define GPU_VISIBILITY_OCCLUSION 0
#endif
struct Instance
{
    float4x4 model,previousMVP;
    float4 centerRadius,lodErrors;
    uint4 batches,metadata;
};
struct Batch {uint indexCount,firstIndex;int vertexOffset;uint instanceOffset;uint4 capacityPadding;};
struct VisibleInstance {float4x4 model,previousMVP;};
struct Frame {float4 planes[6];float4 depthRow,projection;uint4 counts;};
#if GPU_VISIBILITY_OCCLUSION
struct OcclusionFrame {float4x4 viewProjection;float4 viewport;uint4 info;};
bool Occluded(float4 sphere,constant OcclusionFrame& occlusion,texture2d<float> hizImage)
{
    float2 low=1e30f,high=-1e30f;float nearestDepth=1;
    for(uint corner=0;corner<8;++corner)
    {
        float3 signValue=float3((corner&1u)!=0?1:-1,(corner&2u)!=0?1:-1,(corner&4u)!=0?1:-1);
        float4 clip=occlusion.viewProjection*float4(sphere.xyz+signValue*sphere.w,1);
        if(!all(isfinite(clip)) || clip.w<=1e-6f || clip.z<=0) return false;
        float3 ndc=clip.xyz/clip.w;
        float2 pixel=(ndc.xy*float2(.5,-.5)+.5)*occlusion.viewport.xy;
        low=min(low,pixel);high=max(high,pixel);nearestDepth=min(nearestDepth,ndc.z);
    }
    if(high.x<0 || high.y<0 || low.x>=occlusion.viewport.x || low.y>=occlusion.viewport.y) return false;
    low=clamp(floor(low-occlusion.viewport.w),0.f,occlusion.viewport.xy-1);
    high=clamp(ceil(high+occlusion.viewport.w)-1,0.f,occlusion.viewport.xy-1);
    float2 extent=high-low+1;
    int level=clamp(int(ceil(log2(max(extent.x,extent.y))))-2,0,int(occlusion.info.x)-1);
    float footprint=float(4u<<uint(level));
    int2 size=int2(hizImage.get_width(uint(level)),hizImage.get_height(uint(level)));
    int2 first=clamp(int2(floor(low/footprint)),int2(0),size-1);
    int2 last=clamp(int2(floor(high/footprint)),int2(0),size-1);
    float blocker=0;
    for(int y=first.y;y<=last.y;++y) for(int x=first.x;x<=last.x;++x)
    {
        float depth=hizImage.read(uint2(x,y),uint(level)).r;
        if(!isfinite(depth) || depth>=1) return false;
        blocker=max(blocker,depth);
    }
    return isfinite(nearestDepth) && blocker+occlusion.viewport.z<nearestDepth;
}
#endif
kernel void kernelMain(uint3 threadId [[thread_position_in_grid]],
    device const Instance* inputs [[buffer(0)]],device const Batch* batches [[buffer(1)]],
    device VisibleInstance* visible [[buffer(2)]],device atomic_uint* arguments [[buffer(3)]],
    device uint* history [[buffer(4)]],constant Frame& frame [[buffer(5)]],
#if GPU_VISIBILITY_OCCLUSION
    texture2d<float> hizImage [[texture(6)]],constant OcclusionFrame& occlusion [[buffer(7)]],sampler hizSampler [[sampler(8)]],
#endif
    constant uint4& operation [[buffer(15)]])
{
    uint id=threadId.x+threadId.y*operation.y;
    if(operation.x==0)
    {
        if(id<frame.counts.y)
        {
            Batch batch=batches[id];uint offset=id*5;
            atomic_store_explicit(&arguments[offset],batch.indexCount,memory_order_relaxed);
            atomic_store_explicit(&arguments[offset+1],0,memory_order_relaxed);
            atomic_store_explicit(&arguments[offset+2],batch.firstIndex,memory_order_relaxed);
            atomic_store_explicit(&arguments[offset+3],as_type<uint>(batch.vertexOffset),memory_order_relaxed);
            atomic_store_explicit(&arguments[offset+4],operation.w!=0?batch.instanceOffset:0,memory_order_relaxed);
        }
        if(operation.z!=0 && id<frame.counts.x) history[id]=0;
        return;
    }
    if(id>=frame.counts.x) return;
    Instance input=inputs[id];
    bool bounded=(input.metadata.y&1u)!=0 && input.centerRadius.w>=0 && all(isfinite(input.centerRadius));
    if(frame.counts.z!=0 && bounded)
        for(uint plane=0;plane<6;++plane)
        {
            float4 p=frame.planes[plane];
            float distance=dot(p,float4(input.centerRadius.xyz,1));
            float tolerance=1e-5f*(dot(abs(p.xyz),abs(input.centerRadius.xyz))+abs(p.w)+input.centerRadius.w+1);
            if(distance < -input.centerRadius.w-tolerance) return;
        }
    uint selected=0;
    if(frame.counts.w!=0 && bounded)
    {
        float depth=dot(frame.depthRow,float4(input.centerRadius.xyz,1));
        bool perspective=frame.projection.w>.5f;
        float pixels=frame.projection.x*(perspective?1.f/max(depth-input.centerRadius.w,.001f):1.f);
        if((!perspective || depth>input.centerRadius.w+.001f) && isfinite(pixels) && pixels>=0)
        {
            uint previous=min(history[id],input.metadata.x-1);
            for(uint level=1;level<input.metadata.x;++level)
            {
                float error=input.lodErrors[level]*pixels;
                float limit=frame.projection.y*(level>previous?1-frame.projection.z:1+frame.projection.z);
                if(!isfinite(error) || error<0 || error>limit) break;
                selected=level;
            }
        }
    }
    history[id]=selected;
#if GPU_VISIBILITY_OCCLUSION
    if(bounded && Occluded(input.centerRadius,occlusion,hizImage)) return;
#endif
    uint batchId=input.batches[selected];Batch batch=batches[batchId];
    uint index=atomic_fetch_add_explicit(&arguments[batchId*5+1],1,memory_order_relaxed);
    if(index>=batch.capacityPadding.x) return;
    visible[batch.instanceOffset+index]={input.model,input.previousMVP};
}
