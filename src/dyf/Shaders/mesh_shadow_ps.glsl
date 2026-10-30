#version 450
layout(location=0) in vec2 shadowUv;
layout(location=1) in float shadowAlpha;
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
// 반전 변환의 면 방향을 보정하고, Mask 재질만 정점·텍스처 알파와 임곗값을 검사한다.
void main()
{
    bool front=gl_FrontFacing!=((drawConstants.padding1 & 32u)!=0u);
    if((drawConstants.padding1 & 4u)==0u && !front) discard;
    if((drawConstants.padding1 & 3u)==1u)
    {
        float alpha=drawConstants.baseColor.a*shadowAlpha;
        if((drawConstants.textureFlags & 1u)!=0u)
            alpha*=texture(sampler2D(baseColorTexture,materialSampler),shadowUv).a;
        if(alpha<uintBitsToFloat(drawConstants.padding0)) discard; // 최종 알파가 임곗값보다 작으면 픽셀을 버려 그 뒤의 깊이가 보이게 한다.
    }
}
