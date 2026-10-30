#include <metal_stdlib>
using namespace metal;
struct SurfaceDraw {float4x4 model;float4x4 previousMVP;float4x4 normalMatrix;float4 baseColor;float4 materialParams;uint4 flags;float4 alpha;};
struct SurfaceFrame {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4x4 viewProjection;float4 size;float4 ao;float4 lighting;float4 options;uint4 outputs;float4x4 inversePreviousProjection;};
struct SurfaceOutput {float4 position [[position]];float2 uv;float3 worldNormal;float4 worldTangent;float4 currentClip;float4 previousClip;float viewDepth;float4 vertexColor;};
struct SurfaceTargets {float4 normalRoughAO [[color(0)]];float4 albedoMetal [[color(1)]];float4 motionDepth [[color(2)]];};
struct SurfaceTargets1 {float4 first [[color(0)]];};
struct SurfaceTargets2 {float4 first [[color(0)]];float4 second [[color(1)]];};
struct SurfaceValues {float4 values[3];};
static float2 surfaceOctEncode(float3 n)
{
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    if(n.z<0)n.xy=(1.0f-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1);
    return n.xy*0.5f+0.5f;
}
static SurfaceValues SurfaceEvaluate(SurfaceOutput input,bool front,
    device const SurfaceDraw* draws,constant SurfaceFrame& frame,constant uint4& index,
    texture2d<float> baseTexture,texture2d<float> materialTexture,texture2d<float> normalTexture,
    texture2d<float> aoTexture,texture2d<float> emissiveTexture,sampler materialSampler)
{
    SurfaceDraw draw=draws[index.x];uint flags=draw.flags.x;SurfaceValues output={};
    bool needNormal=(frame.outputs.w&1u)!=0u,needMaterial=(frame.outputs.w&2u)!=0u,needMotion=(frame.outputs.w&4u)!=0u;
    float4 base=draw.baseColor*input.vertexColor;
    if((needMaterial || draw.flags.y==1) && (flags&1)!=0)base*=baseTexture.sample(materialSampler,input.uv);
    if((flags&64u)!=0u)base.rgb=draw.baseColor.rgb;
    float3 n=float3(0,0,1);float roughness=0.04f;
    float2 mr=draw.materialParams.xy;
    if((needNormal || needMaterial) && (flags&2)!=0)mr*=materialTexture.sample(materialSampler,input.uv).bg;
    if(needNormal)
    {
        n=normalize(input.worldNormal);
        if((flags&4)!=0 && abs(draw.materialParams.z)>0.0001f)
        {
            float3 t=normalize(input.worldTangent.xyz-n*dot(n,input.worldTangent.xyz));
            float3 b=cross(n,t)*input.worldTangent.w;
            float3 mapped=normalTexture.sample(materialSampler,input.uv).xyz*2.0f-1.0f;
            if((flags&256u)!=0u)mapped.z=sqrt(max(0.0f,1.0f-dot(mapped.xy,mapped.xy)));
            mapped.xy*=draw.materialParams.z;mapped.y*=draw.alpha.y<0?-1.0f:1.0f;n=normalize(t*mapped.x+b*mapped.y+n*mapped.z);
        }
        roughness=clamp(mr.y,0.04f,1.0f);
        float3 normalDx=dfdx(n),normalDy=dfdy(n);
        float normalVariance=0.25f*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
        float filteredAlphaSquared=min(1.0f,roughness*roughness*roughness*roughness+min(2.0f*normalVariance,0.18f));
        roughness=sqrt(sqrt(filteredAlphaSquared));
    }
    if(draw.flags.y==1 && base.a<draw.alpha.x)discard_fragment();
    if(needNormal)
    {
        if(draw.flags.z!=0 && !front)n=-n;
        float authoredAO=1.0f;
        if((flags&8)!=0)authoredAO=mix(1.0f,aoTexture.sample(materialSampler,input.uv).r,draw.materialParams.w);
        // 음수 거칠기는 조명을 받지 않는 재질 표식이며 법선·AO·모션은 보존한다.
        output.values[frame.outputs.x]=float4(surfaceOctEncode(n),draw.alpha.z>.5f?-1.0f:roughness,clamp(authoredAO,0.0f,1.0f));
    }
    if(needMaterial)output.values[frame.outputs.y]=float4(max(base.rgb,0.0f),clamp(mr.x,0.0f,1.0f));
    if(needMotion)
    {
        float4 previousView=frame.inversePreviousProjection*input.previousClip;
        // Match viewDepth without a homogeneous divide, which loses precision at distant perspective depths.
        float previousDepth=-previousView.z;
        bool valid=previousDepth>0.0 && frame.options.w>0.5f && input.previousClip.w>1e-5f && input.currentClip.w>1e-5f;
        float2 currentUV=input.currentClip.xy/input.currentClip.w*float2(0.5f,-0.5f)+0.5f;
        float2 previousUV=valid?input.previousClip.xy/input.previousClip.w*float2(0.5f,-0.5f)+0.5f:currentUV;
        output.values[frame.outputs.z]=float4(currentUV-previousUV,max(input.viewDepth,0.0f),valid?previousDepth:0.0f);
    }
    return output;
}

#define SURFACE_ARGUMENTS SurfaceOutput input [[stage_in]],bool front [[front_facing]], \
    device const SurfaceDraw* draws [[buffer(11)]],constant SurfaceFrame& frame [[buffer(12)]], \
    constant uint4& index [[buffer(15)]],texture2d<float> baseTexture [[texture(0)]], \
    texture2d<float> materialTexture [[texture(1)]],texture2d<float> normalTexture [[texture(2)]], \
    texture2d<float> aoTexture [[texture(3)]],texture2d<float> emissiveTexture [[texture(4)]],sampler materialSampler [[sampler(8)]]
#define SURFACE_EVALUATE SurfaceEvaluate(input,front,draws,frame,index,baseTexture,materialTexture,normalTexture,aoTexture,emissiveTexture,materialSampler)
fragment void surfaceFragment0(SURFACE_ARGUMENTS) { (void)SURFACE_EVALUATE; }
fragment SurfaceTargets1 surfaceFragment1(SURFACE_ARGUMENTS)
{
    SurfaceValues values=SURFACE_EVALUATE;
    return {values.values[0]};
}
fragment SurfaceTargets2 surfaceFragment2(SURFACE_ARGUMENTS)
{
    SurfaceValues values=SURFACE_EVALUATE;
    return {values.values[0],values.values[1]};
}
fragment SurfaceTargets surfaceFragment(SURFACE_ARGUMENTS)
{
    SurfaceValues values=SURFACE_EVALUATE;
    return {values.values[0],values.values[1],values.values[2]};
}
#undef SURFACE_EVALUATE
#undef SURFACE_ARGUMENTS
