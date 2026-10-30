#include <metal_stdlib>
using namespace metal;
// 색상 패스와 같은 필드 배치다. baseColor.a와 선택적인 텍스처 알파를 곱한 뒤 padding0의 float 비트값을 임곗값으로 사용한다.
struct DrawConstants
{
    float4x4 viewProjectionMatrix;
    float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};
struct ShadowOutput {float4 position [[position]]; float2 uv; float alpha;};
// 반전 변환의 면 방향을 보정하고, Mask 재질만 정점·텍스처 알파와 임곗값을 검사한다.
fragment void shadowFragmentShader(
    ShadowOutput input [[stage_in]],
    bool frontFace [[front_facing]],
    constant DrawConstants& constants [[buffer(10)]],
    texture2d<float> baseColorTexture [[texture(0)]],
    sampler materialSampler [[sampler(8)]])
{
    bool front=frontFace!=((constants.padding1 & 32u)!=0u);
    if((constants.padding1 & 4u)==0u && !front) discard_fragment();
    if((constants.padding1 & 3u)==1u)
    {
        float alpha=constants.baseColor.a*input.alpha;
        if((constants.textureFlags & 1u)!=0u) alpha*=baseColorTexture.sample(materialSampler,input.uv).a;
        if(alpha<as_type<float>(constants.padding0)) discard_fragment();
    }
}
