#version 450
#ifndef GPU_VISIBILITY_OCCLUSION
#define GPU_VISIBILITY_OCCLUSION 0
#endif
layout(local_size_x=64,local_size_y=1,local_size_z=1) in;
struct Instance
{
    mat4 model,previousMVP;
    vec4 centerRadius,lodErrors;
    uvec4 batches,metadata;
};
struct Batch {uint indexCount,firstIndex;int vertexOffset;uint instanceOffset;uvec4 capacityPadding;};
struct VisibleInstance {mat4 model,previousMVP;};
layout(std430,set=0,binding=0) readonly buffer Inputs {Instance inputs[];};
layout(std430,set=0,binding=1) readonly buffer Batches {Batch batches[];};
layout(std430,set=0,binding=2) writeonly buffer Visible {VisibleInstance visible[];};
layout(std430,set=0,binding=3) buffer Arguments {uint arguments[];};
layout(std430,set=0,binding=4) buffer History {uint history[];};
layout(std140,set=0,binding=5) uniform Frame {vec4 planes[6];vec4 depthRow,projection;uvec4 counts;} frame;
layout(push_constant) uniform Operation {uvec4 values;} operation;
#if GPU_VISIBILITY_OCCLUSION
layout(set=0,binding=6) uniform texture2D hizImage;
layout(std140,set=0,binding=7) uniform OcclusionFrame {mat4 viewProjection;vec4 viewport;uvec4 info;} occlusion;
layout(set=0,binding=8) uniform sampler hizSampler;
#endif
bool Finite(float value) {return !isnan(value) && !isinf(value);}
#if GPU_VISIBILITY_OCCLUSION
bool Occluded(vec4 sphere)
{
    vec2 low=vec2(1e30),high=vec2(-1e30);float nearestDepth=1;
    for(uint corner=0;corner<8;++corner)
    {
        vec3 signValue=vec3((corner&1u)!=0?1:-1,(corner&2u)!=0?1:-1,(corner&4u)!=0?1:-1);
        vec4 clip=occlusion.viewProjection*vec4(sphere.xyz+signValue*sphere.w,1);
        if(any(isnan(clip)) || any(isinf(clip)) || clip.w<=1e-6 || clip.z<=0) return false;
        vec3 ndc=clip.xyz/clip.w;
        vec2 pixel=(ndc.xy*vec2(.5,-.5)+.5)*occlusion.viewport.xy;
        low=min(low,pixel);high=max(high,pixel);nearestDepth=min(nearestDepth,ndc.z);
    }
    if(high.x<0 || high.y<0 || low.x>=occlusion.viewport.x || low.y>=occlusion.viewport.y) return false;
    low=clamp(floor(low-occlusion.viewport.w),vec2(0),occlusion.viewport.xy-1);
    high=clamp(ceil(high+occlusion.viewport.w)-1,vec2(0),occlusion.viewport.xy-1);
    vec2 extent=high-low+1;
    int level=clamp(int(ceil(log2(max(extent.x,extent.y))))-2,0,int(occlusion.info.x)-1);
    float footprint=float(4u<<uint(level));
    ivec2 size=textureSize(sampler2D(hizImage,hizSampler),level);
    ivec2 first=clamp(ivec2(floor(low/footprint)),ivec2(0),size-1);
    ivec2 last=clamp(ivec2(floor(high/footprint)),ivec2(0),size-1);
    float blocker=0;
    // 사각형 전체의 셀을 읽고 최원 깊이도 확실히 앞에 있을 때만 제거한다.
    for(int y=first.y;y<=last.y;++y) for(int x=first.x;x<=last.x;++x)
    {
        float depth=texelFetch(sampler2D(hizImage,hizSampler),ivec2(x,y),level).r;
        if(!Finite(depth) || depth>=1) return false;
        blocker=max(blocker,depth);
    }
    return Finite(nearestDepth) && blocker+occlusion.viewport.z<nearestDepth;
}
#endif
void main()
{
    uint id=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*operation.values.y;
    if(operation.values.x==0)
    {
        if(id<frame.counts.y)
        {
            Batch batch=batches[id];uint offset=id*5;
            arguments[offset]=batch.indexCount;arguments[offset+1]=0;
            arguments[offset+2]=batch.firstIndex;arguments[offset+3]=uint(batch.vertexOffset);
            arguments[offset+4]=operation.values.w!=0?batch.instanceOffset:0;
        }
        if(operation.values.z!=0 && id<frame.counts.x) history[id]=0;
        return;
    }
    if(id>=frame.counts.x) return;
    Instance value=inputs[id];
    bool bounded=(value.metadata.y&1u)!=0 && value.centerRadius.w>=0 &&
        !any(isnan(value.centerRadius)) && !any(isinf(value.centerRadius));
    if(frame.counts.z!=0 && bounded)
        for(uint plane=0;plane<6;++plane)
        {
            vec4 p=frame.planes[plane];
            float distance=dot(p,vec4(value.centerRadius.xyz,1));
            float tolerance=1e-5*(dot(abs(p.xyz),abs(value.centerRadius.xyz))+abs(p.w)+value.centerRadius.w+1);
            if(distance < -value.centerRadius.w-tolerance) return;
        }
    uint selected=0;
    if(frame.counts.w!=0 && bounded)
    {
        float depth=dot(frame.depthRow,vec4(value.centerRadius.xyz,1));
        bool perspective=frame.projection.w>.5;
        float pixels=frame.projection.x*(perspective?1/max(depth-value.centerRadius.w,.001):1);
        if((!perspective || depth>value.centerRadius.w+.001) && Finite(pixels) && pixels>=0)
        {
            uint previous=min(history[id],value.metadata.x-1);
            for(uint level=1;level<value.metadata.x;++level)
            {
                float error=value.lodErrors[level]*pixels;
                float limit=frame.projection.y*(level>previous?1-frame.projection.z:1+frame.projection.z);
                if(!Finite(error) || error<0 || error>limit) break;
                selected=level;
            }
        }
    }
    history[id]=selected;
#if GPU_VISIBILITY_OCCLUSION
    if(bounded && Occluded(value.centerRadius)) return;
#endif
    uint batchId=value.batches[selected];Batch batch=batches[batchId];
    uint index=atomicAdd(arguments[batchId*5+1],1);
    if(index>=batch.capacityPadding.x) return;
    visible[batch.instanceOffset+index].model=value.model;
    visible[batch.instanceOffset+index].previousMVP=value.previousMVP;
}
