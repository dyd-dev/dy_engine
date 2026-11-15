#version 450
#extension GL_EXT_samplerless_texture_functions : require
#ifdef PROBE_VERTEX
void main()
{
    vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
    gl_Position=vec4(p*2.0-1.0,0,1);
}
#else
layout(set=0,binding=0) uniform utexture2D ownerTexture;
layout(location=0) out vec4 color;
void main()
{
    uvec2 owner=texelFetch(ownerTexture,ivec2(gl_FragCoord.xy),0).rg;
    color=vec4(vec2(owner)/255.0,0,1);
}
#endif
