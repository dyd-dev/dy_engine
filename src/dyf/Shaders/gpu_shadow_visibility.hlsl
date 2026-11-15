struct Input {float4 minimum,maximum;uint4 ids;};
struct Batch {uint indexCount,firstIndex;int vertexOffset;uint instanceOffset;uint4 capacityPadding;};
struct View {column_major float4x4 projection;float4 receiverPlanes[18];uint4 info;};
struct Instance {column_major float4x4 model,previous;};
StructuredBuffer<Input> inputs:register(t0);
StructuredBuffer<Batch> batches:register(t1);
StructuredBuffer<View> views:register(t2);
StructuredBuffer<Instance> models:register(t3);
RWStructuredBuffer<Instance> visible:register(u4);
RWStructuredBuffer<uint> arguments:register(u5);
cbuffer Operation:register(b15) {uint4 counts;uint4 settings;}
bool visibleToLight(Input item,float4x4 model,View view) {
    if(!(item.minimum.w>0) || !all(isfinite(item.minimum)) || !all(isfinite(item.maximum)) ||
        any(item.minimum.xyz>item.maximum.xyz))return true;
    for(uint i=0;i<4;++i)if(!all(isfinite(model[i])) || !all(isfinite(view.projection[i])))return true;
    if(model[3][0]!=0 || model[3][1]!=0 || model[3][2]!=0 || model[3][3]!=1)return true;
    float3 center=(item.minimum.xyz+item.maximum.xyz)*.5,halfSize=(item.maximum.xyz-item.minimum.xyz)*.5;
    float3 localMagnitude=max(abs(item.minimum.xyz),abs(item.maximum.xyz));
    float3 worldCenter=mul(model,float4(center,1)).xyz;
    float3 extent=mul(abs((float3x3)model),halfSize);
    float3 magnitude=abs(float3(model[0][3],model[1][3],model[2][3]))+mul(abs((float3x3)model),localMagnitude);
    float3 low=worldCenter-extent,high=worldCenter+extent;
    if(!all(isfinite(low)) || !all(isfinite(high)) || any(magnitude>1e21))return true;
    for(uint i=0;i<6+view.info.x;++i) {
        float4 plane,scalePlane;
        if(i<6) {
            plane=i==4?view.projection[2]:view.projection[3]+(i%2==0?1.0:-1.0)*view.projection[i/2];
            scalePlane=i==4?abs(view.projection[2]):abs(view.projection[3])+abs(view.projection[i/2]);
            if(all(plane.xyz==0))return true;
        } else {plane=view.receiverPlanes[i-6];scalePlane=abs(plane);}
        float distance=plane.w+dot(plane.xyz,float3(plane.x>=0?high.x:low.x,plane.y>=0?high.y:low.y,plane.z>=0?high.z:low.z));
        float scale=scalePlane.w+dot(scalePlane.xyz,magnitude);
        if(!all(isfinite(plane)) || !isfinite(distance) || !isfinite(scale) || scale>1e32)return true;
        // Cover f32 plane conversion, transformed-bound rounding and fused operations.
        if(distance < -1e-4*max(1.0,scale))return false;
    }
    return true;
}
[numthreads(64,1,1)]
void main(uint3 thread:SV_DispatchThreadID) {
    uint id=thread.x+thread.y*counts.y;
    if(counts.x==0) {
        if(id>=counts.w*settings.x)return;
        uint view=id/counts.w,b=id%counts.w;Batch batch=batches[b];uint offset=id*5;
        arguments[offset]=batch.indexCount;arguments[offset+1]=0;arguments[offset+2]=batch.firstIndex;
        arguments[offset+3]=uint(batch.vertexOffset);arguments[offset+4]=view*settings.y+batch.instanceOffset;
    } else {
        if(id>=counts.z*settings.x)return;
        uint view=id/counts.z;if(views[view].info.y==0)return;
        Input item=inputs[id%counts.z];Instance instance=models[item.ids.x];
        if(!visibleToLight(item,instance.model,views[view]))return;
        Batch batch=batches[item.ids.y];uint offset=(view*counts.w+item.ids.y)*5;
        uint slot;InterlockedAdd(arguments[offset+1],1,slot);
        if(slot>=batch.capacityPadding.x) {InterlockedMin(arguments[offset+1],batch.capacityPadding.x);return;}
        visible[view*settings.y+batch.instanceOffset+slot]=instance;
    }
}
