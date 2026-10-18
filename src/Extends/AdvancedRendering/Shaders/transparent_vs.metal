#include <metal_stdlib>
using namespace metal;
struct TransparentDraw {float4x4 model;float4x4 normalMatrix;float4 baseColor;float4 emissive;float4 material;float4 optics;float4 attenuation;uint4 flags;};
struct TransparentFrame {float4x4 viewProjection;float4x4 view;float4x4 inverseViewProjection;float4 camera;float4 sh[9];float4 settings;float4 screen;float4 probePosition;float4 probeBoxMin;float4 probeBoxMax;float4 specularColor;};
struct TransparentInput {float3 position [[attribute(0)]];float3 normal [[attribute(1)]];float2 uv [[attribute(2)]];float4 tangent [[attribute(3)]];};
struct TransparentVertex {float4 position [[position,invariant]];float2 uv;float3 worldPosition;float3 worldNormal;float4 worldTangent;};
float3 TransparentVertexNormalizeOr(float3 value,float3 fallback)
{
    float scale=max(max(abs(value.x),abs(value.y)),abs(value.z));
    if(!(scale>0) || !all(isfinite(value)))return fallback;
    return normalize(value/scale);
}
vertex TransparentVertex transparentVertex(TransparentInput input [[stage_in]],device const TransparentDraw* draws [[buffer(11)]],
    constant TransparentFrame& frame [[buffer(12)]],constant uint4& index [[buffer(15)]])
{
    TransparentDraw draw=draws[index.x];const float4 world=draw.model*float4(input.position,1);
    const float4 clip=frame.viewProjection*world;TransparentVertex output;output.position=clip;
    output.uv=input.uv;output.worldPosition=world.xyz;output.worldNormal=TransparentVertexNormalizeOr((draw.normalMatrix*float4(input.normal,0)).xyz,float3(0,0,1));
    float3 t=(draw.model*float4(input.tangent.xyz,0)).xyz;t-=output.worldNormal*dot(output.worldNormal,t);
    float3 fallback=normalize(cross(abs(output.worldNormal.z)<.99f?float3(0,0,1):float3(0,1,0),output.worldNormal));
    output.worldTangent=float4(TransparentVertexNormalizeOr(t,fallback),(input.tangent.w<0?-1.0f:1.0f)*((draw.flags.z&2u)!=0u?-1.0f:1.0f));
    return output;
}
