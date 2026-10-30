cbuffer Frame : register(b15) {float4 camera;float4 projection;uint4 controls;};
StructuredBuffer<float4> bounds : register(t0);
RWStructuredBuffer<uint> visibleIds : register(u1);
RWStructuredBuffer<uint> arguments : register(u2);
[numthreads(64,1,1)]
void main(uint3 thread:SV_DispatchThreadID)
{
    uint id=thread.x;
    if(controls.y==0)
    {
        if(id==0) {arguments[0]=3;arguments[1]=0;arguments[2]=0;arguments[3]=0;}
        return;
    }
    if(id>=controls.x) return;
    float4 sphere=bounds[id];float phase=.37*float(id);
    sphere.xy+=.25*float2(sin(camera.w+phase),cos(camera.w*.8+phase));
    float3 position=sphere.xyz-camera.xyz;
    float2 margin=sphere.w*sqrt(projection.xy*projection.xy+1);
    bool inside=position.z+sphere.w>=projection.z && position.z-sphere.w<=projection.w &&
        all(abs(position.xy)*projection.xy<=position.z+margin);
    if(!inside) return;
    uint slot;InterlockedAdd(arguments[1],1,slot);
    visibleIds[slot]=id;
}
