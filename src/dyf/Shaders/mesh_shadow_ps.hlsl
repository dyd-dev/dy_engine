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
// 면 방향을 먼저 검사한다. 알파 모드 하위 2비트가 Mask(1)일 때만 텍스처 알파와 임곗값을 검사한다.
void main(float2 uv : TEXCOORD0, bool frontFace : SV_IsFrontFace)
{
    if((padding1 & 4u)==0u && !frontFace) discard; // 양면 비트가 꺼진 재질의 뒷면은 그림자 깊이에도 남기지 않는다.
    if((padding1 & 3u)==1u) // Mask 재질만 잘라내기 검사를 수행한다. Opaque·Blend의 분류와 제출 정책은 호출측이 결정한다.
    {
        float alpha=baseColor.a;
        if((textureFlags & 1u)!=0u) alpha*=baseColorTexture.Sample(materialSampler,uv).a; // 기본색 이미지가 있는 재질만 이미지의 알파를 곱한다.
        // clip은 alpha-cutoff가 음수일 때 픽셀을 버린다. 임곗값과 같은 알파는 남는다.
        clip(alpha-asfloat(padding0));
    }
}
