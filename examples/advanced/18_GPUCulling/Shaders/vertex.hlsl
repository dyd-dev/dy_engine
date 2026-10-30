cbuffer Frame : register(b15) {float4 camera;float4 projection;uint4 controls;};
StructuredBuffer<float4> bounds : register(t0);
StructuredBuffer<uint> visibleIds : register(t1);
struct Output {float4 position:SV_Position;float3 color:COLOR0;};
Output main(uint vertex:SV_VertexID,uint instance:SV_InstanceID)
{
    const float2 vertices[3]={float2(-.8,-.6),float2(.8,-.6),float2(0,.95)};
    uint id=visibleIds[instance];
    float4 sphere=bounds[id];float phase=.37*float(id);
    sphere.xy+=.25*float2(sin(camera.w+phase),cos(camera.w*.8+phase));
    float3 position=sphere.xyz-camera.xyz;
    position.xy+=vertices[vertex]*sphere.w;
    float depth=projection.w/(projection.w-projection.z);
    Output output;
    output.position=float4(position.xy*projection.xy,depth*(position.z-projection.z),position.z);
    output.color=float3(.15,.35,.65)+float3(.75,.55,.3)*frac(float(id)*float3(.13,.37,.07));
    return output;
}
