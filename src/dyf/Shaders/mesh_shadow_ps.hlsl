Texture2D<float4> baseColorTexture : register(t0,space0);
SamplerState materialSampler : register(s8,space0);
// 색상 패스와 같은 필드 배치다. baseColor.a와 선택적인 텍스처 알파를 곱한 뒤 padding0의 float 비트값을 임곗값으로 사용한다.
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
// 반전 변환의 면 방향을 보정하고, Mask 재질만 정점·텍스처 알파와 임곗값을 검사한다.
void main(float2 uv : TEXCOORD0, float vertexAlpha : TEXCOORD1, bool frontFace : SV_IsFrontFace)
{
    bool front=frontFace!=((padding1 & 32u)!=0u);
    if((padding1 & 4u)==0u && !front) discard;
    if((padding1 & 3u)==1u)
    {
        float alpha=baseColor.a*vertexAlpha;
        if((textureFlags & 1u)!=0u) alpha*=baseColorTexture.Sample(materialSampler,uv).a;
        // 임곗값보다 작은 알파만 버리며 같은 값은 남긴다.
        clip(alpha-asfloat(padding0));
    }
}
