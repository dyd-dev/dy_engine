Texture2D<float4> baseColorTexture : register(t0,space0);
SamplerState materialSampler : register(s8,space0);
cbuffer DrawConstants : register(b10,space0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};
void main(float2 uv : TEXCOORD0, float vertexAlpha : TEXCOORD1, bool frontFace : SV_IsFrontFace)
{
    bool front=frontFace!=((padding1 & 32u)!=0u);
    if((padding1 & 4u)==0u && !front) discard;
    if((padding1 & 3u)==1u)
    {
        float alpha=baseColor.a*vertexAlpha;
        if((textureFlags & 1u)!=0u) alpha*=baseColorTexture.Sample(materialSampler,uv).a;
        clip(alpha-asfloat(padding0));
    }
}
