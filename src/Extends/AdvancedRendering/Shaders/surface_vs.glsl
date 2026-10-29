#version 450
invariant gl_Position;
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 texcoord;
layout(location=3) in vec4 tangent;
layout(location=4) in vec3 previousPosition;
layout(location=5) in mat4 instanceModel;
layout(location=9) in mat4 instancePreviousMVP;
layout(location=13) in vec4 color;
struct Draw {mat4 model;mat4 previousMVP;mat4 normalMatrix;vec4 baseColor;vec4 materialParams;uvec4 flags;vec4 alpha;};
layout(std430,set=0,binding=11) readonly buffer Draws {Draw draws[];};
layout(set=0,binding=12) uniform Frame {mat4 view;mat4 projection;mat4 inverseProjection;mat4 viewProjection;vec4 size;vec4 ao;vec4 lighting;vec4 options;uvec4 outputs;mat4 inversePreviousProjection;} frame;
layout(push_constant) uniform Index {uvec4 index;};
layout(location=0) out vec2 uv;
layout(location=1) out vec3 worldNormal;
layout(location=2) out vec4 worldTangent;
layout(location=3) out vec4 currentClip;
layout(location=4) out vec4 previousClip;
layout(location=5) out float viewDepth;
layout(location=6) out vec4 vertexColor;
vec3 SurfaceNormalizeOr(vec3 value,vec3 fallback)
{
    float scale=max(max(abs(value.x),abs(value.y)),abs(value.z));
    if(!(scale>0.0) || any(isnan(value)) || any(isinf(value)))return fallback;
    return normalize(value/scale);
}
mat3 SurfaceNormalMatrix(mat4 model)
{
    vec3 a=model[0].xyz,b=model[1].xyz,c=model[2].xyz;
    float scale=max(max(max(abs(a.x),abs(a.y)),abs(a.z)),
        max(max(max(abs(b.x),abs(b.y)),abs(b.z)),max(max(abs(c.x),abs(c.y)),abs(c.z))));
    if(!(scale>0.0) || isinf(scale))return mat3(1.0);
    a/=scale;b/=scale;c/=scale;
    float det=dot(a,cross(b,c));
    if(det==0.0 || isnan(det))return mat3(1.0);
    // Match Main: normalize cofactors, omitting their common positive inverse scale.
    float orientation=det<0.0?-1.0:1.0;
    return mat3(cross(b,c)*orientation,cross(c,a)*orientation,cross(a,b)*orientation);
}
void main()
{
    Draw draw=draws[index.x];
    mat4 model=index.y!=0u?instanceModel:draw.model;
    precise vec4 world=model*vec4(position,1.0);
    precise vec4 clipPosition=frame.viewProjection*world;
    // 이전 변형 정점과 이전 객체·카메라 행렬을 함께 사용한다.
    currentClip=clipPosition;previousClip=vec4(0.0);viewDepth=0.0;
    if((frame.outputs.w&4u)!=0u)
    {
        mat4 previousMVP=index.y!=0u?instancePreviousMVP:draw.previousMVP;
        previousClip=index.z!=0u?previousMVP*vec4(previousPosition,1.0):vec4(0.0);
        viewDepth=-(frame.view*world).z;
    }
    gl_Position=clipPosition;uv=texcoord;vertexColor=color;
    worldNormal=vec3(0,0,1);worldTangent=vec4(1,0,0,1);
    if((frame.outputs.w&1u)!=0u)
    {
        mat3 normalMatrix=SurfaceNormalMatrix(model);
        worldNormal=SurfaceNormalizeOr(normalMatrix*normal,vec3(0,0,1));
        vec3 t=mat3(model)*tangent.xyz;
        t-=worldNormal*dot(worldNormal,t);
        vec3 fallback=SurfaceNormalizeOr(cross(abs(worldNormal.z)<0.999?vec3(0,0,1):vec3(0,1,0),worldNormal),vec3(1,0,0));
        worldTangent=vec4(SurfaceNormalizeOr(t,fallback),tangent.w*(determinant(mat3(model))<0?-1.0:1.0));
    }
}
