#include <metal_stdlib>
using namespace metal;
struct Frame {float4 camera,projection;uint4 controls;};
kernel void cullObjects(constant Frame& frame [[buffer(15)]],device const float4* bounds [[buffer(0)]],
    device uint* visibleIds [[buffer(1)]],device atomic_uint* arguments [[buffer(2)]],uint id [[thread_position_in_grid]])
{
    if(frame.controls.y==0)
    {
        if(id==0)
        {
            atomic_store_explicit(arguments,3u,memory_order_relaxed);
            atomic_store_explicit(arguments+1,0u,memory_order_relaxed);
            atomic_store_explicit(arguments+2,0u,memory_order_relaxed);
            atomic_store_explicit(arguments+3,0u,memory_order_relaxed);
        }
        return;
    }
    if(id>=frame.controls.x) return;
    float4 sphere=bounds[id];const float phase=.37f*float(id);
    sphere.xy+=.25f*float2(sin(frame.camera.w+phase),cos(frame.camera.w*.8f+phase));
    const float3 position=sphere.xyz-frame.camera.xyz;
    const float2 margin=sphere.w*sqrt(frame.projection.xy*frame.projection.xy+1.f);
    const bool inside=position.z+sphere.w>=frame.projection.z && position.z-sphere.w<=frame.projection.w &&
        all(abs(position.xy)*frame.projection.xy<=position.z+margin);
    if(!inside) return;
    const uint slot=atomic_fetch_add_explicit(arguments+1,1u,memory_order_relaxed);
    visibleIds[slot]=id;
}
struct Output {float4 position [[position]];float3 color;};
vertex Output vertexMain(uint vertex [[vertex_id]],uint instance [[instance_id]],
    constant Frame& frame [[buffer(15)]],device const float4* bounds [[buffer(0)]],
    device const uint* visibleIds [[buffer(1)]])
{
    const float2 vertices[3]={float2(-.8f,-.6f),float2(.8f,-.6f),float2(0,.95f)};
    const uint id=visibleIds[instance];
    float4 sphere=bounds[id];const float phase=.37f*float(id);
    sphere.xy+=.25f*float2(sin(frame.camera.w+phase),cos(frame.camera.w*.8f+phase));
    float3 position=sphere.xyz-frame.camera.xyz;position.xy+=vertices[vertex]*sphere.w;
    const float depth=frame.projection.w/(frame.projection.w-frame.projection.z);
    const float3 color=float3(.15f,.35f,.65f)+float3(.75f,.55f,.3f)*fract(float(id)*float3(.13f,.37f,.07f));
    return {float4(position.xy*frame.projection.xy,depth*(position.z-frame.projection.z),position.z),color};
}
fragment float4 fragmentMain(Output input [[stage_in]]) {return float4(input.color,1);}
