#ifndef GPU_VISIBILITY_OCCLUSION
#define GPU_VISIBILITY_OCCLUSION 0
#endif
struct Instance
{
    column_major float4x4 model,previousMVP;
    float4 centerRadius,lodErrors;
    uint4 batches,metadata;
};
struct Batch {uint indexCount,firstIndex;int vertexOffset;uint instanceOffset;uint4 capacityPadding;};
struct VisibleInstance {column_major float4x4 model,previousMVP;};
StructuredBuffer<Instance> inputs:register(t0,space0);
StructuredBuffer<Batch> batches:register(t1,space0);
RWStructuredBuffer<VisibleInstance> visible:register(u2,space0);
RWStructuredBuffer<uint> arguments:register(u3,space0);
RWStructuredBuffer<uint> history:register(u4,space0);
cbuffer Frame:register(b5,space0) {float4 planes[6];float4 depthRow,projection;uint4 counts;};
cbuffer Operation:register(b15,space0) {uint4 operation;};
#if GPU_VISIBILITY_OCCLUSION
Texture2D<float> hizImage:register(t6,space0);
cbuffer OcclusionFrame:register(b7,space0) {column_major float4x4 occlusionVP;float4 viewport;uint4 hizInfo;};
SamplerState hizSampler:register(s8,space0);
bool Occluded(float4 sphere)
{
    float2 low=1e30,high=-1e30;float nearestDepth=1;
    for(uint corner=0;corner<8;++corner)
    {
        float3 signValue=float3((corner&1u)!=0?1:-1,(corner&2u)!=0?1:-1,(corner&4u)!=0?1:-1);
        float4 clip=mul(occlusionVP,float4(sphere.xyz+signValue*sphere.w,1));
        if(!all(isfinite(clip)) || clip.w<=1e-6 || clip.z<=0) return false;
        float3 ndc=clip.xyz/clip.w;
        float2 pixel=(ndc.xy*float2(.5,-.5)+.5)*viewport.xy;
        low=min(low,pixel);high=max(high,pixel);nearestDepth=min(nearestDepth,ndc.z);
    }
    if(high.x<0 || high.y<0 || low.x>=viewport.x || low.y>=viewport.y) return false;
    low=clamp(floor(low-viewport.w),0,viewport.xy-1);
    high=clamp(ceil(high+viewport.w)-1,0,viewport.xy-1);
    float2 extent=high-low+1;
    int level=clamp(int(ceil(log2(max(extent.x,extent.y))))-2,0,int(hizInfo.x)-1);
    float footprint=float(4u<<uint(level));
    uint width,height,levels;hizImage.GetDimensions(uint(level),width,height,levels);
    int2 first=clamp(int2(floor(low/footprint)),int2(0,0),int2(width,height)-1);
    int2 last=clamp(int2(floor(high/footprint)),int2(0,0),int2(width,height)-1);
    float blocker=0;
    for(int y=first.y;y<=last.y;++y) for(int x=first.x;x<=last.x;++x)
    {
        float depth=hizImage.Load(int3(x,y,level));
        if(!isfinite(depth) || depth>=1) return false;
        blocker=max(blocker,depth);
    }
    return isfinite(nearestDepth) && blocker+viewport.z<nearestDepth;
}
#endif
[numthreads(64,1,1)]
void main(uint3 threadId:SV_DispatchThreadID)
{
    uint id=threadId.x+threadId.y*operation.y;
    if(operation.x==0)
    {
        if(id<counts.y)
        {
            Batch batch=batches[id];uint offset=id*5;
            arguments[offset]=batch.indexCount;arguments[offset+1]=0;
            arguments[offset+2]=batch.firstIndex;arguments[offset+3]=asuint(batch.vertexOffset);arguments[offset+4]=0;
        }
        if(operation.z!=0 && id<counts.x) history[id]=0;
        return;
    }
    if(id>=counts.x) return;
    Instance input=inputs[id];
    bool bounded=(input.metadata.y&1u)!=0 && input.centerRadius.w>=0 && all(isfinite(input.centerRadius));
    if(counts.z!=0 && bounded)
        for(uint plane=0;plane<6;++plane)
        {
            float4 p=planes[plane];
            float distance=dot(p,float4(input.centerRadius.xyz,1));
            float tolerance=1e-5*(dot(abs(p.xyz),abs(input.centerRadius.xyz))+abs(p.w)+input.centerRadius.w+1);
            if(distance < -input.centerRadius.w-tolerance) return;
        }
    uint selected=0;
    if(counts.w!=0 && bounded)
    {
        float depth=dot(depthRow,float4(input.centerRadius.xyz,1));
        bool perspective=projection.w>.5;
        float pixels=projection.x*(perspective?1/max(depth-input.centerRadius.w,.001):1);
        if((!perspective || depth>input.centerRadius.w+.001) && isfinite(pixels) && pixels>=0)
        {
            uint previous=min(history[id],input.metadata.x-1);
            for(uint level=1;level<input.metadata.x;++level)
            {
                float error=input.lodErrors[level]*pixels;
                float limit=projection.y*(level>previous?1-projection.z:1+projection.z);
                if(!isfinite(error) || error<0 || error>limit) break;
                selected=level;
            }
        }
    }
    history[id]=selected;
#if GPU_VISIBILITY_OCCLUSION
    if(bounded && Occluded(input.centerRadius)) return;
#endif
    uint batchId=input.batches[selected];Batch batch=batches[batchId];
    uint index;InterlockedAdd(arguments[batchId*5+1],1,index);
    if(index>=batch.capacityPadding.x) return;
    VisibleInstance result;result.model=input.model;result.previousMVP=input.previousMVP;
    visible[batch.instanceOffset+index]=result;
}
