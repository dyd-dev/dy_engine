#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform texture2D sourceDepth;
layout(set=0,binding=1) uniform sampler depthSampler;
layout(push_constant) uniform Settings {vec4 reconstruction;} settings;
void main()
{
    float depth=textureLod(sampler2D(sourceDepth,depthSampler),uv,0).r;
    vec4 r=settings.reconstruction;
    // 저장 깊이에서 카메라 전방 거리를 복원한다. 깊이 1 이상은 far 거리로 두고, 아래에서 시선 길이를 곱해 실제 방사 거리로 바꾼다.
    float viewDistance=depth>=1 ? r.w : r.z/((1-depth)+depth*(r.z/r.w));
    vec2 ray=(uv*vec2(2,-2)+vec2(-1,1))*r.xy;
    // 숫자를 정수로 반올림하지 않고 float 비트 패턴을 그대로 읽는다. 하위 바이트부터 R·G·B·A에 넣고 255로 나누어 UNORM 색 채널로 전달한다.
    uint bits=floatBitsToUint(viewDistance*length(vec3(ray,1)));
    color=vec4(bits&255u,(bits>>8)&255u,(bits>>16)&255u,(bits>>24)&255u)/255.0;
}
