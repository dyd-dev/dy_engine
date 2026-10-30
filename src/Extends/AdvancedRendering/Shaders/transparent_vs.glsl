#version 450
invariant gl_Position;
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 texcoord;
layout(location=3) in vec4 tangent;
layout(location=8) in vec4 color;
struct Draw {mat4 model;mat4 normalMatrix;vec4 baseColor;vec4 emissive;vec4 material;vec4 optics;vec4 attenuation;uvec4 flags;};
layout(std430,set=0,binding=11) readonly buffer Draws {Draw draws[];};
layout(set=0,binding=12) uniform Frame {mat4 viewProjection;mat4 view;mat4 inverseViewProjection;vec4 camera;vec4 sh[9];vec4 settings;vec4 screen;vec4 probePosition;vec4 probeBoxMin;vec4 probeBoxMax;vec4 specularColor;} frame;
layout(push_constant) uniform Index {uvec4 index;};
layout(location=0) out vec2 uv;
layout(location=1) out vec3 worldPosition;
layout(location=2) out vec3 worldNormal;
layout(location=3) out vec4 worldTangent;
layout(location=4) out vec4 vertexColor;
vec3 TransparentVertexNormalizeOr(vec3 value,vec3 fallback)
{
    float scale=max(max(abs(value.x),abs(value.y)),abs(value.z));
    if(!(scale>0) || any(isnan(value)) || any(isinf(value)))return fallback;
    return normalize(value/scale);
}
void main()
{
    Draw draw=draws[index.x];precise vec4 world=draw.model*vec4(position,1);
    precise vec4 clip=frame.viewProjection*world;gl_Position=clip;
    uv=texcoord;vertexColor=color;worldPosition=world.xyz;worldNormal=TransparentVertexNormalizeOr(mat3(draw.normalMatrix)*normal,vec3(0,0,1));
    vec3 t=mat3(draw.model)*tangent.xyz;t-=worldNormal*dot(worldNormal,t);
    vec3 fallback=normalize(cross(abs(worldNormal.z)<.99?vec3(0,0,1):vec3(0,1,0),worldNormal));
    worldTangent=vec4(TransparentVertexNormalizeOr(t,fallback),(tangent.w<0?-1:1)*((draw.flags.z&2u)!=0u?-1:1));
}
