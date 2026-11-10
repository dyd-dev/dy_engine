#version 450
#ifndef GPU_RANGE_OCCLUSION
#define GPU_RANGE_OCCLUSION 0
#endif
layout(local_size_x=64) in;
struct Range {vec4 minimum,maximum;uvec4 draw;};
struct Instance {mat4 model,previousMVP;};
layout(std430,set=0,binding=0) readonly buffer Ranges {Range ranges[];};
layout(std430,set=0,binding=1) readonly buffer SourceInstances {Instance sourceInstances[];};
layout(std430,set=0,binding=2) readonly buffer SourceArguments {uint sourceArguments[];};
layout(std430,set=0,binding=3) readonly buffer SourceKeys {uvec2 sourceKeys[];};
layout(std430,set=0,binding=4) writeonly buffer Instances {Instance instances[];};
layout(std430,set=0,binding=5) buffer Arguments {uint arguments[];};
layout(std430,set=0,binding=6) writeonly buffer Keys {uvec2 keys[];};
layout(std140,set=0,binding=7) uniform Frame {vec4 planes[6];vec4 depthRow,projection,lodScreenSizes;uvec4 counts;} frame;
layout(push_constant) uniform Operation {uvec4 operation;};
bool finite4(vec4 v) {return !any(isnan(v)) && !any(isinf(v));}
#if GPU_RANGE_OCCLUSION
layout(set=0,binding=8) uniform texture2D hizImage;
layout(std140,set=0,binding=9) uniform OcclusionFrame {mat4 viewProjection;vec4 viewport;uvec4 info;} occlusion;
layout(set=0,binding=10) uniform sampler hizSampler;
bool occludedRange(vec3 minimum,vec3 maximum) {
    if(occlusion.info.x==0u)return false;
    vec2 low=vec2(1e30),high=vec2(-1e30);float nearestDepth=1;
    for(uint corner=0;corner<8;++corner) {
        vec3 position=mix(minimum,maximum,bvec3((corner&1u)!=0u,(corner&2u)!=0u,(corner&4u)!=0u));
        vec4 clip=occlusion.viewProjection*vec4(position,1);
        if(!finite4(clip) || clip.w<=1e-6 || clip.z<=0)return false;
        vec3 ndc=clip.xyz/clip.w;vec2 pixel=(ndc.xy*vec2(.5,-.5)+.5)*occlusion.viewport.xy;
        if(!finite4(vec4(ndc,0)) || !finite4(vec4(pixel,0,0)))return false;
        low=min(low,pixel);high=max(high,pixel);nearestDepth=min(nearestDepth,ndc.z);
    }
    if(high.x<0 || high.y<0 || low.x>=occlusion.viewport.x || low.y>=occlusion.viewport.y)return false;
    low=clamp(floor(low-occlusion.viewport.w),vec2(0),occlusion.viewport.xy-1);
    high=clamp(ceil(high+occlusion.viewport.w)-1,vec2(0),occlusion.viewport.xy-1);
    vec2 extent=high-low+1;
    int level=clamp(int(ceil(log2(max(extent.x,extent.y))))-2,0,int(occlusion.info.x)-1);
    float footprint=float(4u<<uint(level));
    ivec2 size=textureSize(sampler2D(hizImage,hizSampler),level);
    ivec2 first=clamp(ivec2(floor(low/footprint)),ivec2(0),size-1);
    ivec2 last=clamp(ivec2(floor(high/footprint)),ivec2(0),size-1);
    float blocker=0;
    for(int y=first.y;y<=last.y;++y)for(int x=first.x;x<=last.x;++x) {
        float depth=texelFetch(sampler2D(hizImage,hizSampler),ivec2(x,y),level).r;
        if(isnan(depth) || isinf(depth) || depth<0 || depth>=1)return false;
        blocker=max(blocker,depth);
    }
    return blocker+occlusion.viewport.z<nearestDepth;
}
#endif
bool visibleRange(Range item,mat4 model) {
#if !GPU_RANGE_OCCLUSION
    if(frame.counts.z==0)return true;
#endif
    if(!(item.minimum.w>0) || !finite4(item.minimum) || !finite4(item.maximum) ||
        any(greaterThan(item.minimum.xyz,item.maximum.xyz)))return true;
    for(uint i=0;i<4;++i)if(!finite4(model[i]))return true;
    if(model[0].w!=0 || model[1].w!=0 || model[2].w!=0 || model[3].w!=1)return true;
    vec3 center=(item.minimum.xyz+item.maximum.xyz)*.5,halfSize=(item.maximum.xyz-item.minimum.xyz)*.5;
    vec3 localMagnitude=max(abs(item.minimum.xyz),abs(item.maximum.xyz));
    vec3 worldCenter=(model*vec4(center,1)).xyz;
    vec3 extent=abs(model[0].xyz)*halfSize.x+abs(model[1].xyz)*halfSize.y+abs(model[2].xyz)*halfSize.z;
    vec3 magnitude=abs(model[3].xyz)+abs(model[0].xyz)*localMagnitude.x+
        abs(model[1].xyz)*localMagnitude.y+abs(model[2].xyz)*localMagnitude.z;
    vec3 low=worldCenter-extent,high=worldCenter+extent;
    if(!finite4(vec4(low,0)) || !finite4(vec4(high,0)) || any(greaterThan(magnitude,vec3(1e21))))return true;
    if(frame.counts.z!=0)for(uint i=0;i<6;++i) {
        vec4 plane=frame.planes[i];
        float distance=plane.w+dot(plane.xyz,mix(low,high,greaterThanEqual(plane.xyz,vec3(0))));
        float scale=abs(plane.w)+dot(abs(plane.xyz),magnitude);
        if(!finite4(plane) || !finite4(vec4(distance,scale,0,0)) || scale>1e32)return true;
        // Same conservative transform-rounding margin as shadow range culling.
        if(distance < -1e-4*max(1.0,scale))return false;
    }
#if GPU_RANGE_OCCLUSION
    // Inflate transformed bounds before projection, then use the existing HiZ pixel/depth margins.
    vec3 padding=1e-4*max(vec3(1),magnitude);
    if(occludedRange(low-padding,high+padding))return false;
#endif
    return true;
}
void main() {
    uint id=gl_WorkGroupID.x+gl_WorkGroupID.y*operation.y;
    if(id>=operation.x)return;
    uint lane=gl_LocalInvocationID.x;Range item=ranges[id];uint src=item.draw.x*5,dst=id*5;
    if(lane==0) {
        arguments[dst]=item.draw.z;arguments[dst+1]=0;
        arguments[dst+2]=sourceArguments[src+2]+item.draw.y;arguments[dst+3]=sourceArguments[src+3];
        arguments[dst+4]=item.draw.w;
    }
    memoryBarrierBuffer();barrier();
    uint count=sourceArguments[src+1],first=sourceArguments[src+4];
    for(uint i=lane;i<count;i+=64) {
        Instance value=sourceInstances[first+i];
        if(!visibleRange(item,value.model))continue;
        uint row=item.draw.w+atomicAdd(arguments[dst+1],1);
        instances[row]=value;keys[row]=sourceKeys[first+i]+uvec2(0,item.draw.y/3u);
    }
}
