#include <metal_stdlib>
using namespace metal;
struct Output{float4 position [[position]];float3 color;};
vertex Output vertexMain(uint id [[vertex_id]],uint instance [[instance_id]],constant float4& value [[buffer(15)]]){
float2 corners[6]={float2(-1,-1),float2(1,-1),float2(-1,1),float2(-1,1),float2(1,-1),float2(1,1)};
bool nearFace=(instance&1)==1,distant=instance>=2;float n=value.z,f=value.w;
float z=distant?10000.f+(nearFace?0.f:.002f):(nearFace?2.f:3.f);float2 ndc=float2(distant?.48f:-.48f,nearFace?.06f:-.06f)+corners[id]*float2(.34,.5);
float depth=value.x>.5?(f*n-n*z)/(f-n):(f*z-f*n)/(f-n);return {float4(ndc*z,depth,z),nearFace?float3(.1,.85,.35):float3(.9,.12,.2)};}
fragment float4 fragmentMain(Output input [[stage_in]]){return float4(input.color,1);}
