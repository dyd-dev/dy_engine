#version 450
layout(location=0) in vec2 shadowUv;
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
    if((drawConstants.padding1 & 4u)==0u && !gl_FrontFacing) discard;
    if((drawConstants.padding1 & 3u)==1u)
    {
        float alpha=drawConstants.baseColor.a;
        if((drawConstants.textureFlags & 1u)!=0u)
            alpha*=texture(sampler2D(baseColorTexture,materialSampler),shadowUv).a;
        if(alpha<uintBitsToFloat(drawConstants.padding0)) discard;
    }
}
