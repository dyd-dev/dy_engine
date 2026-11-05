#version 450
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
bool visibleRange(Range item,mat4 model) {
    if(frame.counts.z==0 || !(item.minimum.w>0) || !finite4(item.minimum) || !finite4(item.maximum) ||
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
    for(uint i=0;i<6;++i) {
        vec4 plane=frame.planes[i];
        float distance=plane.w+dot(plane.xyz,mix(low,high,greaterThanEqual(plane.xyz,vec3(0))));
        float scale=abs(plane.w)+dot(abs(plane.xyz),magnitude);
        if(!finite4(plane) || !finite4(vec4(distance,scale,0,0)) || scale>1e32)return true;
        // Same conservative transform-rounding margin as shadow range culling.
        if(distance < -1e-4*max(1.0,scale))return false;
    }
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
