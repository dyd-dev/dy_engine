#include <metal_stdlib>
using namespace metal;
struct SurfaceDraw {float4x4 model;float4x4 previousMVP;float4x4 normalMatrix;float4 baseColor;float4 materialParams;uint4 flags;float4 alpha;};
struct SurfaceFrame {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4x4 viewProjection;float4 size;float4 ao;float4 lighting;float4 options;};
struct SurfaceInput {
    float3 position [[attribute(0)]];float3 normal [[attribute(1)]];float2 uv [[attribute(2)]];float4 tangent [[attribute(3)]];
    float3 previousPosition [[attribute(4)]];
    float4 model0 [[attribute(5)]];float4 model1 [[attribute(6)]];float4 model2 [[attribute(7)]];float4 model3 [[attribute(8)]];
    float4 previous0 [[attribute(9)]];float4 previous1 [[attribute(10)]];float4 previous2 [[attribute(11)]];float4 previous3 [[attribute(12)]];
};
struct SurfaceOutput {float4 position [[position,invariant]];float2 uv;float3 worldNormal;float4 worldTangent;float4 currentClip;float4 previousClip;float viewDepth;};
vertex SurfaceOutput surfaceVertex(SurfaceInput input [[stage_in]],device const SurfaceDraw* draws [[buffer(11)]],
    constant SurfaceFrame& frame [[buffer(12)]],constant uint4& index [[buffer(15)]])
{
    SurfaceDraw draw=draws[index.x];
    float4x4 model=index.y?float4x4(input.model0,input.model1,input.model2,input.model3):draw.model;
    float4x4 previousMVP=index.y?float4x4(input.previous0,input.previous1,input.previous2,input.previous3):draw.previousMVP;
    float3x3 normalMatrix(draw.normalMatrix[0].xyz,draw.normalMatrix[1].xyz,draw.normalMatrix[2].xyz);
    if(index.y)
    {
        float3 a=model[0].xyz,b=model[1].xyz,c=model[2].xyz;
        float det=dot(a,cross(b,c));
        normalMatrix=abs(det)>1e-20f?float3x3(cross(b,c),cross(c,a),cross(a,b))/det:float3x3(1.0f);
    }
    const float4 world=model*float4(input.position,1.0f);
    float4 viewPosition=frame.view*world;SurfaceOutput output;
    const float4 clipPosition=frame.viewProjection*world;
    // 같은 표면의 이전 변형과 이전 카메라 좌표로 재투영한다.
    output.currentClip=clipPosition;output.previousClip=index.z?previousMVP*float4(input.previousPosition,1.0f):float4(0);
    output.position=clipPosition;output.uv=input.uv;output.viewDepth=-viewPosition.z;
    output.worldNormal=normalize(normalMatrix*input.normal);
    float3 t=(model*float4(input.tangent.xyz,0)).xyz;
    t-=output.worldNormal*dot(output.worldNormal,t);
    if(dot(t,t)<1e-8f)t=cross(abs(output.worldNormal.z)<0.99f?float3(0,0,1):float3(0,1,0),output.worldNormal);
    float3x3 model3(model[0].xyz,model[1].xyz,model[2].xyz);
    output.worldTangent=float4(normalize(t),input.tangent.w*(determinant(model3)<0?-1.0f:1.0f));
    return output;
}
