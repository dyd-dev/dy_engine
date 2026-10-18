#version 450
invariant gl_Position;
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 texcoord;
layout(location=3) in vec4 tangent;
layout(location=4) in vec3 previousPosition;
layout(location=5) in mat4 instanceModel;
layout(location=9) in mat4 instancePreviousMVP;
struct Draw {mat4 model;mat4 previousMVP;mat4 normalMatrix;vec4 baseColor;vec4 materialParams;uvec4 flags;vec4 alpha;};
layout(std430,set=0,binding=11) readonly buffer Draws {Draw draws[];};
layout(set=0,binding=12) uniform Frame {mat4 view;mat4 projection;mat4 inverseProjection;mat4 viewProjection;vec4 size;vec4 ao;vec4 lighting;vec4 options;} frame;
layout(push_constant) uniform Index {uvec4 index;};
layout(location=0) out vec2 uv;
layout(location=1) out vec3 worldNormal;
layout(location=2) out vec4 worldTangent;
layout(location=3) out vec4 currentClip;
layout(location=4) out vec4 previousClip;
layout(location=5) out float viewDepth;
void main()
{
    Draw draw=draws[index.x];
    mat4 model=index.y!=0u?instanceModel:draw.model;
    mat4 previousMVP=index.y!=0u?instancePreviousMVP:draw.previousMVP;
    mat3 normalMatrix=mat3(draw.normalMatrix);
    if(index.y!=0u)
    {
        mat3 linear=mat3(model);
        normalMatrix=abs(determinant(linear))>1e-20?transpose(inverse(linear)):mat3(1.0);
    }
    precise vec4 world=model*vec4(position,1.0);
    vec4 viewPosition=frame.view*world;
    precise vec4 clipPosition=frame.viewProjection*world;
    // 이전 변형 정점과 이전 객체·카메라 행렬을 함께 사용한다.
    currentClip=clipPosition;previousClip=index.z!=0u?previousMVP*vec4(previousPosition,1.0):vec4(0.0);
    gl_Position=clipPosition;uv=texcoord;viewDepth=-viewPosition.z;
    worldNormal=normalize(normalMatrix*normal);
    vec3 t=mat3(model)*tangent.xyz;
    t-=worldNormal*dot(worldNormal,t);
    if(dot(t,t)<1e-8)t=cross(abs(worldNormal.z)<0.99?vec3(0,0,1):vec3(0,1,0),worldNormal);
    worldTangent=vec4(normalize(t),tangent.w*(determinant(mat3(model))<0?-1.0:1.0));
}
