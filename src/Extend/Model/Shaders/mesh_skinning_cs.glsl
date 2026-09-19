#version 450
#extension GL_GOOGLE_include_directive : require
#include "ModelShaderLayout.inc"
#include "Skinning.glsl"

layout(local_size_x=64) in;
layout(std430,set=0,binding=14) readonly buffer SourceVertices { float values[]; } sourceVertices;
layout(std430,set=0,binding=15) writeonly buffer SkinnedVertices { float values[]; } skinnedVertices;
layout(push_constant) uniform SkinningDispatch
{
    uint vertexCount;
    uint influenceOffset;
    uint paletteOffset;
    uint reserved;
} dispatchData;

void main()
{
    uint vertex=gl_GlobalInvocationID.x;
    if(vertex>=dispatchData.vertexCount) return;
    uint base=vertex*12;
    mat4 skin,normalSkin;
    LoadSkinning(vertex,dispatchData.influenceOffset,dispatchData.paletteOffset,skin,normalSkin);
    vec3 position=vec3(sourceVertices.values[base],sourceVertices.values[base+1],sourceVertices.values[base+2]);
    vec3 normal=vec3(sourceVertices.values[base+3],sourceVertices.values[base+4],sourceVertices.values[base+5]);
    vec3 tangent=vec3(sourceVertices.values[base+8],sourceVertices.values[base+9],sourceVertices.values[base+10]);
    position=(skin*vec4(position,1)).xyz;
    normal=normalize((normalSkin*vec4(normal,0)).xyz);
    tangent=(skin*vec4(tangent,0)).xyz;
    tangent=normalize(tangent-normal*dot(normal,tangent));
    for(uint i=0;i<3;++i)
    {
        skinnedVertices.values[base+i]=position[i];
        skinnedVertices.values[base+3+i]=normal[i];
        skinnedVertices.values[base+8+i]=tangent[i];
    }
    skinnedVertices.values[base+6]=sourceVertices.values[base+6];
    skinnedVertices.values[base+7]=sourceVertices.values[base+7];
    skinnedVertices.values[base+11]=sourceVertices.values[base+11]*(determinant(mat3(skin))<0 ? -1 : 1);
}
