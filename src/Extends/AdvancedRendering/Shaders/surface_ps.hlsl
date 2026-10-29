struct Draw {float4x4 model;float4x4 previousMVP;float4x4 normalMatrix;float4 baseColor;float4 materialParams;uint4 flags;float4 alpha;};
StructuredBuffer<Draw> draws:register(t11);
cbuffer Frame:register(b12) {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4x4 viewProjection;float4 size;float4 ao;float4 lighting;float4 options;uint4 outputs;float4x4 inversePreviousProjection;};
cbuffer Index:register(b15) {uint4 drawIndex;};
Texture2D<float4> baseTexture:register(t0),materialTexture:register(t1),normalTexture:register(t2),aoTexture:register(t3),emissiveTexture:register(t4);
SamplerState materialSampler:register(s8);
struct Input {float4 position:SV_Position;float2 uv:TEXCOORD0;float3 worldNormal:TEXCOORD1;float4 worldTangent:TEXCOORD2;float4 currentClip:TEXCOORD3;float4 previousClip:TEXCOORD4;float viewDepth:TEXCOORD5;float4 vertexColor:TEXCOORD6;};
struct Output {float4 normalRoughAO:SV_Target0;float4 albedoMetal:SV_Target1;float4 motionDepth:SV_Target2;};
float2 octEncode(float3 n)
{
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    if(n.z<0)n.xy=(1.0-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);
    return n.xy*0.5+0.5;
}
Output main(Input input,bool front:SV_IsFrontFace)
{
    Draw draw=draws[drawIndex.x];uint flags=draw.flags.x;Output output;
    bool needNormal=(outputs.w&1u)!=0u,needMaterial=(outputs.w&2u)!=0u,needMotion=(outputs.w&4u)!=0u;
    float4 base=draw.baseColor*input.vertexColor;
    if((needMaterial || draw.flags.y==1) && (flags&1)!=0)base*=baseTexture.Sample(materialSampler,input.uv);
    if((flags&64u)!=0u)base.rgb=draw.baseColor.rgb;
    float3 n=float3(0,0,1);float roughness=0.04;
    float2 mr=draw.materialParams.xy;
    if((needNormal || needMaterial) && (flags&2)!=0)mr*=materialTexture.Sample(materialSampler,input.uv).bg;
    if(needNormal)
    {
        n=normalize(input.worldNormal);
        if((flags&4)!=0 && abs(draw.materialParams.z)>0.0001)
        {
            float3 t=normalize(input.worldTangent.xyz-n*dot(n,input.worldTangent.xyz));
            float3 b=cross(n,t)*input.worldTangent.w;
            float3 mapped=normalTexture.Sample(materialSampler,input.uv).xyz*2.0-1.0;
            if((flags&256u)!=0u)mapped.z=sqrt(max(0.0f,1.0f-dot(mapped.xy,mapped.xy)));
            mapped.xy*=draw.materialParams.z;mapped.y*=draw.alpha.y<0?-1.0:1.0;n=normalize(t*mapped.x+b*mapped.y+n*mapped.z);
        }
        roughness=clamp(mr.y,0.04,1.0);
        float3 normalDx=ddx(n),normalDy=ddy(n);
        float normalVariance=0.25*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
        float filteredAlphaSquared=min(1.0,roughness*roughness*roughness*roughness+min(2.0*normalVariance,0.18));
        roughness=sqrt(sqrt(filteredAlphaSquared));
    }
    if(draw.flags.y==1 && base.a<draw.alpha.x)discard;
    float4 values[3]={float4(0,0,0,0),float4(0,0,0,0),float4(0,0,0,0)};
    if(needNormal)
    {
        if(draw.flags.z!=0 && !front)n=-n;
        float authoredAO=1.0;
        if((flags&8)!=0)authoredAO=lerp(1.0,aoTexture.Sample(materialSampler,input.uv).r,draw.materialParams.w);
        // 음수 거칠기는 조명을 받지 않는 재질 표식이며 법선·AO·모션은 보존한다.
        values[outputs.x]=float4(octEncode(n),draw.alpha.z>.5?-1.0:roughness,saturate(authoredAO));
    }
    if(needMaterial)values[outputs.y]=float4(max(base.rgb,0),saturate(mr.x));
    if(needMotion)
    {
        float4 previousView=mul(inversePreviousProjection,input.previousClip);
        // Match viewDepth without a homogeneous divide, which loses precision at distant perspective depths.
        float previousDepth=-previousView.z;
        bool valid=previousDepth>0.0 && options.w>0.5 && input.previousClip.w>1e-5 && input.currentClip.w>1e-5;
        float2 currentUV=input.currentClip.xy/input.currentClip.w*float2(0.5,-0.5)+0.5;
        float2 previousUV=valid?input.previousClip.xy/input.previousClip.w*float2(0.5,-0.5)+0.5:currentUV;
        values[outputs.z]=float4(currentUV-previousUV,max(input.viewDepth,0.0),valid?previousDepth:0.0);
    }
    output.normalRoughAO=values[0];output.albedoMetal=values[1];output.motionDepth=values[2];
    return output;
}
