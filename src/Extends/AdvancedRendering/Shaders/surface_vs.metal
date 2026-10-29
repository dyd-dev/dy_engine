#include <metal_stdlib>
using namespace metal;
struct SurfaceDraw {float4x4 model;float4x4 previousMVP;float4x4 normalMatrix;float4 baseColor;float4 materialParams;uint4 flags;float4 alpha;};
struct SurfaceFrame {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4x4 viewProjection;float4 size;float4 ao;float4 lighting;float4 options;uint4 outputs;};
struct SurfaceInput {
    float3 position [[attribute(0)]];float3 normal [[attribute(1)]];float2 uv [[attribute(2)]];float4 tangent [[attribute(3)]];
    float3 previousPosition [[attribute(4)]];
    float4 model0 [[attribute(5)]];float4 model1 [[attribute(6)]];float4 model2 [[attribute(7)]];float4 model3 [[attribute(8)]];
    float4 previous0 [[attribute(9)]];float4 previous1 [[attribute(10)]];float4 previous2 [[attribute(11)]];float4 previous3 [[attribute(12)]];
    float4 color [[attribute(13)]];
};
struct SurfaceOutput {float4 position [[position,invariant]];float2 uv;float3 worldNormal;float4 worldTangent;float4 currentClip;float4 previousClip;float viewDepth;float4 vertexColor;};
float3 SurfaceNormalizeOr(float3 value,float3 fallback)
{
    float scale=max(max(abs(value.x),abs(value.y)),abs(value.z));
    if(!(scale>0.0f) || !all(isfinite(value)))return fallback;
    return normalize(value/scale);
}
float3x3 SurfaceNormalMatrix(float4x4 model)
{
    float3 a=model[0].xyz,b=model[1].xyz,c=model[2].xyz;
    float scale=max(max(max(abs(a.x),abs(a.y)),abs(a.z)),
        max(max(max(abs(b.x),abs(b.y)),abs(b.z)),max(max(abs(c.x),abs(c.y)),abs(c.z))));
    if(!(scale>0.0f) || !isfinite(scale))return float3x3(1.0f);
    a/=scale;b/=scale;c/=scale;
    float det=dot(a,cross(b,c));
    if(det==0.0f || !isfinite(det))return float3x3(1.0f);
    // Match Main: normalize cofactors, omitting their common positive inverse scale.
    float orientation=det<0.0f?-1.0f:1.0f;
    return float3x3(cross(b,c)*orientation,cross(c,a)*orientation,cross(a,b)*orientation);
}
vertex SurfaceOutput surfaceVertex(SurfaceInput input [[stage_in]],device const SurfaceDraw* draws [[buffer(11)]],
    constant SurfaceFrame& frame [[buffer(12)]],constant uint4& index [[buffer(15)]])
{
    SurfaceDraw draw=draws[index.x];
    float4x4 model=index.y?float4x4(input.model0,input.model1,input.model2,input.model3):draw.model;
    const float4 world=model*float4(input.position,1.0f);
    SurfaceOutput output={};
    const float4 clipPosition=frame.viewProjection*world;
    // 같은 표면의 이전 변형과 이전 카메라 좌표로 재투영한다.
    output.currentClip=clipPosition;
    if((frame.outputs.w&4u)!=0u)
    {
        float4x4 previousMVP=index.y?float4x4(input.previous0,input.previous1,input.previous2,input.previous3):draw.previousMVP;
        output.previousClip=index.z?previousMVP*float4(input.previousPosition,1.0f):float4(0);
        output.viewDepth=-(frame.view*world).z;
    }
    output.position=clipPosition;output.uv=input.uv;output.vertexColor=input.color;
    if((frame.outputs.w&1u)!=0u)
    {
        float3x3 normalMatrix=SurfaceNormalMatrix(model);
        output.worldNormal=SurfaceNormalizeOr(normalMatrix*input.normal,float3(0,0,1));
        float3 t=(model*float4(input.tangent.xyz,0)).xyz;
        t-=output.worldNormal*dot(output.worldNormal,t);
        float3 fallback=SurfaceNormalizeOr(cross(abs(output.worldNormal.z)<0.999f?float3(0,0,1):float3(0,1,0),output.worldNormal),float3(1,0,0));
        float3x3 model3(model[0].xyz,model[1].xyz,model[2].xyz);
        output.worldTangent=float4(SurfaceNormalizeOr(t,fallback),input.tangent.w*(determinant(model3)<0?-1.0f:1.0f));
    }
    return output;
}
