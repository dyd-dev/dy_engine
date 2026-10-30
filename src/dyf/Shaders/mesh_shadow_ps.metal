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
struct ShadowOutput {float4 position [[position]]; float2 uv;};
// 면 방향을 먼저 검사한다. 알파 모드 하위 2비트가 Mask(1)일 때만 텍스처 알파와 임곗값을 검사한다.
fragment void shadowFragmentShader(
    ShadowOutput input [[stage_in]],
    bool frontFace [[front_facing]],
    constant DrawConstants& constants [[buffer(10)]],
    texture2d<float> baseColorTexture [[texture(0)]],
    sampler materialSampler [[sampler(8)]])
{
    if((constants.padding1 & 4u)==0u && !frontFace) discard_fragment(); // 양면 비트가 꺼진 재질의 뒷면은 그림자 깊이에도 남기지 않는다.
    if((constants.padding1 & 3u)==1u) // Mask 재질만 잘라내기 검사를 수행한다. Opaque·Blend의 분류와 제출 정책은 호출측이 결정한다.
    {
        float alpha=constants.baseColor.a;
        if((constants.textureFlags & 1u)!=0u) alpha*=baseColorTexture.sample(materialSampler,input.uv).a; // 기본색 이미지가 있는 재질만 이미지의 알파를 곱한다.
        if(alpha<as_type<float>(constants.padding0)) discard_fragment(); // 최종 알파가 임곗값보다 작으면 픽셀을 버려 그 뒤의 깊이가 보이게 한다.
    }
}
