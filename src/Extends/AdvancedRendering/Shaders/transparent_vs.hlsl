struct Draw {column_major float4x4 model;column_major float4x4 normalMatrix;float4 baseColor;float4 emissive;float4 material;float4 optics;float4 attenuation;uint4 flags;};
StructuredBuffer<Draw> draws:register(t11);
cbuffer Frame:register(b12) {column_major float4x4 viewProjection;column_major float4x4 view;column_major float4x4 inverseViewProjection;float4 camera;float4 sh[9];float4 settings;float4 screen;float4 probePosition;float4 probeBoxMin;float4 probeBoxMax;float4 specularColor;};
cbuffer Index:register(b15) {uint4 drawIndex;};
struct Input {float3 position:TEXCOORD0;float3 normal:TEXCOORD1;float2 uv:TEXCOORD2;float4 tangent:TEXCOORD3;};
struct Output {precise float4 position:SV_Position;float2 uv:TEXCOORD0;float3 worldPosition:TEXCOORD1;float3 worldNormal:TEXCOORD2;float4 worldTangent:TEXCOORD3;};
float3 TransparentVertexNormalizeOr(float3 value,float3 fallback)
{
    float scale=max(max(abs(value.x),abs(value.y)),abs(value.z));
    if(!(scale>0) || !all(isfinite(value)))return fallback;
    return normalize(value/scale);
}
Output main(Input input)
{
    Draw draw=draws[drawIndex.x];precise float4 world=mul(draw.model,float4(input.position,1));
    precise float4 clip=mul(viewProjection,world);Output output;output.position=clip;
    output.uv=input.uv;output.worldPosition=world.xyz;output.worldNormal=TransparentVertexNormalizeOr(mul((float3x3)draw.normalMatrix,input.normal),float3(0,0,1));
    float3 t=mul((float3x3)draw.model,input.tangent.xyz);t-=output.worldNormal*dot(output.worldNormal,t);
    float3 fallback=normalize(cross(abs(output.worldNormal.z)<.99?float3(0,0,1):float3(0,1,0),output.worldNormal));
    output.worldTangent=float4(TransparentVertexNormalizeOr(t,fallback),(input.tangent.w<0?-1:1)*((draw.flags.z&2u)!=0u?-1:1));
    return output;
}
