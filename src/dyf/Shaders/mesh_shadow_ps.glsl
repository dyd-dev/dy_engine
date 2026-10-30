#version 450
layout(location=0) in vec2 shadowUv;
layout(set=0,binding=0) uniform texture2D baseColorTexture;
layout(set=0,binding=8) uniform sampler materialSampler;
// 색상 패스와 같은 필드 배치다. baseColor.a와 선택적인 텍스처 알파를 곱한 뒤 padding0의 float 비트값을 임곗값으로 사용한다.
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
// 면 방향을 먼저 검사한다. 알파 모드 하위 2비트가 Mask(1)일 때만 텍스처 알파와 임곗값을 검사한다.
void main()
{
    if((drawConstants.padding1 & 4u)==0u && !gl_FrontFacing) discard; // 양면 비트가 꺼진 재질의 뒷면은 그림자 깊이에도 남기지 않는다.
    if((drawConstants.padding1 & 3u)==1u) // Mask 재질만 잘라내기 검사를 수행한다. Opaque·Blend의 분류와 제출 정책은 호출측이 결정한다.
    {
        float alpha=drawConstants.baseColor.a;
        if((drawConstants.textureFlags & 1u)!=0u) // 기본색 이미지가 있는 재질만 이미지의 알파를 곱한다.
            alpha*=texture(sampler2D(baseColorTexture,materialSampler),shadowUv).a;
        if(alpha<uintBitsToFloat(drawConstants.padding0)) discard; // 최종 알파가 임곗값보다 작으면 픽셀을 버려 그 뒤의 깊이가 보이게 한다.
    }
}
