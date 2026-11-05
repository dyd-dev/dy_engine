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
bool visibleRange(Range item,column_major float4x4 model) {
    if(counts.z==0 || !(item.minimum.w>0) || !finite4(item.minimum) || !finite4(item.maximum) ||
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
    for(uint i=0;i<6;++i) {
        float4 plane=planes[i];
        float distance=plane.w+dot(plane.xyz,float3(plane.x>=0?high.x:low.x,plane.y>=0?high.y:low.y,plane.z>=0?high.z:low.z));
        float scale=abs(plane.w)+dot(abs(plane.xyz),magnitude);
        if(!finite4(plane) || !finite4(float4(distance,scale,0,0)) || scale>1e32)return true;
        // Same conservative transform-rounding margin as shadow range culling.
        if(distance < -1e-4*max(1.0,scale))return false;
    }
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
