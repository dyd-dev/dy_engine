#include <metal_stdlib>
using namespace metal;
struct Output {float4 position [[position]];float2 uv;};
struct Settings {float4 reconstruction;};
// 정점 버퍼 없이 정점 번호 0·1·2로 화면 전체를 덮는 삼각형과 UV를 만든다.
vertex Output environmentDepthVertex(uint id [[vertex_id]])
{
    Output output;output.uv=float2((id<<1)&2,id&2);
    output.position=float4(output.uv*float2(2,-2)+float2(-1,1),0,1);return output;
}
fragment float4 environmentDepthFragment(Output input [[stage_in]],depth2d<float> sourceDepth [[texture(0)]],
    sampler depthSampler [[sampler(1)]],constant Settings& settings [[buffer(15)]])
{
    float depth=sourceDepth.sample(depthSampler,input.uv,level(0));
    float4 r=settings.reconstruction;
    // 저장 깊이에서 카메라 전방 거리를 복원한다. 깊이 1 이상은 far 거리로 두고, 아래에서 시선 길이를 곱해 실제 방사 거리로 바꾼다.
    float viewDistance=depth>=1 ? r.w : r.z/((1-depth)+depth*(r.z/r.w));
    float2 ray=(input.uv*float2(2,-2)+float2(-1,1))*r.xy;
    // 숫자를 정수로 반올림하지 않고 float 비트 패턴을 그대로 읽는다. 하위 바이트부터 R·G·B·A에 넣고 255로 나누어 UNORM 색 채널로 전달한다.
    uint bits=as_type<uint>(viewDistance*length(float3(ray,1)));
    return float4(bits&255u,(bits>>8)&255u,(bits>>16)&255u,(bits>>24)&255u)/255.0;
}
