#version 450
layout(location=0) in vec2 shadowUv;
layout(location=1) in float shadowAlpha;
layout(set=0,binding=0) uniform texture2D baseColorTexture;
layout(set=0,binding=8) uniform sampler materialSampler;
layout(push_constant) uniform DrawConstants
{
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    vec4 emissiveColor;
    vec4 baseColor;
    vec4 materialParams;
} drawConstants;
void main()
{
    bool front=gl_FrontFacing!=((drawConstants.padding1 & 32u)!=0u);
    if((drawConstants.padding1 & 4u)==0u && !front) discard;
    if((drawConstants.padding1 & 3u)==1u)
    {
        float alpha=drawConstants.baseColor.a*shadowAlpha;
        if((drawConstants.textureFlags & 1u)!=0u)
            alpha*=texture(sampler2D(baseColorTexture,materialSampler),shadowUv).a;
        if(alpha<uintBitsToFloat(drawConstants.padding0)) discard;
    }
}
