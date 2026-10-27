#version 450
layout(location=0) in vec2 uv;
layout(location=1) in vec3 worldNormal;
layout(location=2) in vec4 worldTangent;
layout(location=3) in vec4 currentClip;
layout(location=4) in vec4 previousClip;
layout(location=5) in float viewDepth;
layout(location=6) in vec4 vertexColor;
layout(location=0) out vec4 normalRoughAO;
layout(location=1) out vec4 albedoMetal;
layout(location=2) out vec4 motionDepth;
struct Draw {mat4 model;mat4 previousMVP;mat4 normalMatrix;vec4 baseColor;vec4 materialParams;uvec4 flags;vec4 alpha;};
layout(std430,set=0,binding=11) readonly buffer Draws {Draw draws[];};
layout(set=0,binding=12) uniform Frame {mat4 view;mat4 projection;mat4 inverseProjection;mat4 viewProjection;vec4 size;vec4 ao;vec4 lighting;vec4 options;} frame;
layout(push_constant) uniform Index {uvec4 index;};
layout(set=0,binding=0) uniform texture2D baseTexture;
layout(set=0,binding=1) uniform texture2D materialTexture;
layout(set=0,binding=2) uniform texture2D normalTexture;
layout(set=0,binding=3) uniform texture2D aoTexture;
layout(set=0,binding=4) uniform texture2D emissiveTexture;
layout(set=0,binding=8) uniform sampler materialSampler;
vec2 octEncode(vec3 n)
{
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    if(n.z<0)n.xy=(1.0-abs(n.yx))*vec2(n.x>=0?1:-1,n.y>=0?1:-1);
    return n.xy*0.5+0.5;
}
void main()
{
    Draw draw=draws[index.x];uint flags=draw.flags.x;
    vec4 base=draw.baseColor*vertexColor;
    if((flags&1u)!=0u)base*=texture(sampler2D(baseTexture,materialSampler),uv);
    if((flags&64u)!=0u)base.rgb=draw.baseColor.rgb;
    vec3 n=normalize(worldNormal);
    if((flags&4u)!=0u && abs(draw.materialParams.z)>0.0001)
    {
        vec3 t=normalize(worldTangent.xyz-n*dot(n,worldTangent.xyz));
        vec3 b=cross(n,t)*worldTangent.w;
        vec3 mapped=texture(sampler2D(normalTexture,materialSampler),uv).xyz*2.0-1.0;
        if((flags&256u)!=0u)mapped.z=sqrt(max(0.0,1.0-dot(mapped.xy,mapped.xy)));
        mapped.xy*=draw.materialParams.z;mapped.y*=draw.alpha.y<0?-1.0:1.0;n=normalize(mat3(t,b,n)*mapped);
    }
    vec2 mr=draw.materialParams.xy;
    if((flags&2u)!=0u)mr*=texture(sampler2D(materialTexture,materialSampler),uv).bg;
    float roughness=clamp(mr.y,0.04,1.0);
    // Keep the same filtered lobe as MainForward for IBL and screen-space rays.
    // Derivatives use the mapped normal before discard or per-face orientation.
    vec3 normalDx=dFdx(n),normalDy=dFdy(n);
    float normalVariance=0.25*(dot(normalDx,normalDx)+dot(normalDy,normalDy));
    float filteredAlphaSquared=min(1.0,roughness*roughness*roughness*roughness+min(2.0*normalVariance,0.18));
    roughness=sqrt(sqrt(filteredAlphaSquared));
    if(draw.flags.y==1u && base.a<draw.alpha.x)discard;
    if(draw.flags.z!=0u && !gl_FrontFacing)n=-n;
    float authoredAO=1.0;
    if((flags&8u)!=0u)authoredAO=mix(1.0,texture(sampler2D(aoTexture,materialSampler),uv).r,draw.materialParams.w);
    // 음수 거칠기는 조명을 받지 않는 재질 표식이며 법선·AO·모션은 보존한다.
    normalRoughAO=vec4(octEncode(n),draw.alpha.z>.5?-1.0:roughness,clamp(authoredAO,0.0,1.0));
    albedoMetal=vec4(max(base.rgb,vec3(0)),clamp(mr.x,0.0,1.0));
    bool valid=frame.options.w>0.5 && previousClip.w>1e-5 && currentClip.w>1e-5;
    vec2 currentUV=currentClip.xy/currentClip.w*vec2(0.5,-0.5)+0.5;
    vec2 previousUV=valid?previousClip.xy/previousClip.w*vec2(0.5,-0.5)+0.5:currentUV;
    motionDepth=vec4(currentUV-previousUV,max(viewDepth,0.0),valid?previousClip.w:0.0);
}
