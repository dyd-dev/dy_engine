#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler; // Not used but kept to avoid changing C++ descriptor layout right now

void main()
{
    // 정규화된 노멀 벡터 (표면의 방향)
    vec3 normal = normalize(fragNormal);

    // 단순한 가상의 빛 (위에서 아래로 살짝 비스듬히 비추는 빛)
    vec3 lightDir = normalize(vec3(0.5, -1.0, 0.5));

    // 내적(Dot Product)으로 빛과 표면의 각도에 따른 밝기 계산
    float diffuse = max(dot(normal, -lightDir), 0.2); // 최소 밝기 0.2

    // 기본 물체 색상 (약간 푸른빛 도는 회색/하얀색 계열)
    vec3 objectColor = vec3(0.8, 0.8, 0.9);

    // 입체감이 살아있는 최종 색상
    outColor = vec4(objectColor * diffuse, 1.0);
}