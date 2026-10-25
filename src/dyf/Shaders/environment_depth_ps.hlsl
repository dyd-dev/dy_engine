Texture2D<float> sourceDepth:register(t0);
SamplerState depthSampler:register(s1);
cbuffer Settings:register(b15) {float4 reconstruction;};
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target0
{
    float depth=sourceDepth.SampleLevel(depthSampler,uv,0);
    float4 r=reconstruction;
    // 저장 깊이에서 카메라 전방 거리를 복원한다. 깊이 1 이상은 far 거리로 두고, 아래에서 시선 길이를 곱해 실제 방사 거리로 바꾼다.
    float viewDistance=depth>=1 ? r.w : r.z/((1-depth)+depth*(r.z/r.w));
    float2 ray=(uv*float2(2,-2)+float2(-1,1))*r.xy;
    // 숫자를 정수로 반올림하지 않고 float 비트 패턴을 그대로 읽는다. 하위 바이트부터 R·G·B·A에 넣고 255로 나누어 UNORM 색 채널로 전달한다.
    uint bits=asuint(viewDistance*length(float3(ray,1)));
    return float4(bits&255u,(bits>>8)&255u,(bits>>16)&255u,(bits>>24)&255u)/255.0;
}
