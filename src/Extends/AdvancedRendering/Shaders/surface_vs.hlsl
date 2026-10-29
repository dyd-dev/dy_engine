struct Draw {float4x4 model;float4x4 previousMVP;float4x4 normalMatrix;float4 baseColor;float4 materialParams;uint4 flags;float4 alpha;};
StructuredBuffer<Draw> draws:register(t11);
cbuffer Frame:register(b12) {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4x4 viewProjection;float4 size;float4 ao;float4 lighting;float4 options;uint4 outputs;};
cbuffer Index:register(b15) {uint4 drawIndex;};
struct Input {
    float3 position:TEXCOORD0;float3 normal:TEXCOORD1;float2 uv:TEXCOORD2;float4 tangent:TEXCOORD3;
    float3 previousPosition:TEXCOORD4;
    float4 model0:TEXCOORD5;float4 model1:TEXCOORD6;float4 model2:TEXCOORD7;float4 model3:TEXCOORD8;
    float4 previous0:TEXCOORD9;float4 previous1:TEXCOORD10;float4 previous2:TEXCOORD11;float4 previous3:TEXCOORD12;
    float4 color:TEXCOORD13;
};
struct Output {precise float4 position:SV_Position;float2 uv:TEXCOORD0;float3 worldNormal:TEXCOORD1;float4 worldTangent:TEXCOORD2;float4 currentClip:TEXCOORD3;float4 previousClip:TEXCOORD4;float viewDepth:TEXCOORD5;float4 vertexColor:TEXCOORD6;};
float3 SurfaceNormalizeOr(float3 value,float3 fallback)
{
    float scale=max(max(abs(value.x),abs(value.y)),abs(value.z));
    if(!(scale>0.0) || !all(isfinite(value)))return fallback;
    return normalize(value/scale);
}
float3x3 SurfaceNormalMatrix(float4x4 model)
{
    float3 a=float3(model[0][0],model[1][0],model[2][0]);
    float3 b=float3(model[0][1],model[1][1],model[2][1]);
    float3 c=float3(model[0][2],model[1][2],model[2][2]);
    float scale=max(max(max(abs(a.x),abs(a.y)),abs(a.z)),
        max(max(max(abs(b.x),abs(b.y)),abs(b.z)),max(max(abs(c.x),abs(c.y)),abs(c.z))));
    if(!(scale>0.0) || !isfinite(scale))return float3x3(1,0,0,0,1,0,0,0,1);
    a/=scale;b/=scale;c/=scale;
    float det=dot(a,cross(b,c));
    if(det==0.0 || !isfinite(det))return float3x3(1,0,0,0,1,0,0,0,1);
    // Match Main: normalize cofactors, omitting their common positive inverse scale.
    float orientation=det<0.0?-1.0:1.0;
    return transpose(float3x3(cross(b,c)*orientation,cross(c,a)*orientation,cross(a,b)*orientation));
}
Output main(Input input)
{
    Draw draw=draws[drawIndex.x];
    float4x4 model=drawIndex.y!=0?transpose(float4x4(input.model0,input.model1,input.model2,input.model3)):draw.model;
    precise float4 world=mul(model,float4(input.position,1.0));
    Output output=(Output)0;
    precise float4 clipPosition=mul(viewProjection,world);
    // 이전 변형을 알 수 없는 표면은 이 프레임의 시간 누적을 거부한다.
    output.currentClip=clipPosition;
    if((outputs.w&4u)!=0u)
    {
        float4x4 previousMVP=drawIndex.y!=0?transpose(float4x4(input.previous0,input.previous1,input.previous2,input.previous3)):draw.previousMVP;
        output.previousClip=drawIndex.z!=0?mul(previousMVP,float4(input.previousPosition,1.0)):0;
        output.viewDepth=-mul(view,world).z;
    }
    output.position=clipPosition;output.uv=input.uv;output.vertexColor=input.color;
    if((outputs.w&1u)!=0u)
    {
        float3x3 normalMatrix=SurfaceNormalMatrix(model);
        output.worldNormal=SurfaceNormalizeOr(mul(normalMatrix,input.normal),float3(0,0,1));
        float3 t=mul((float3x3)model,input.tangent.xyz);
        t-=output.worldNormal*dot(output.worldNormal,t);
        float3 fallback=SurfaceNormalizeOr(cross(abs(output.worldNormal.z)<0.999?float3(0,0,1):float3(0,1,0),output.worldNormal),float3(1,0,0));
        output.worldTangent=float4(SurfaceNormalizeOr(t,fallback),input.tangent.w*(determinant((float3x3)model)<0?-1.0:1.0));
    }
    return output;
}
