#include <metal_stdlib>
using namespace metal;
struct PresentationVertex
{
    float4 position [[position]];
    float2 uv;
};
// 정점 번호만으로 화면을 덮는 삼각형을 만든다. UV가 0~2인 큰 삼각형을 투영한 뒤 화면에서 잘라 쓰므로 정점 버퍼가 필요 없다.
vertex PresentationVertex presentationVertex(uint id [[vertex_id]])
{
    PresentationVertex output;
    output.uv=float2((id<<1)&2,id&2);
    output.position=float4(output.uv*float2(2,-2)+float2(-1,1),0,1);
    return output;
}
// 전체 화면 UV로 원본의 mip 0을 읽어 출력한다. 여기서는 RGB와 알파를 별도로 수정하지 않는다.
fragment float4 presentationFragment(PresentationVertex input [[stage_in]],
    texture2d<float> sourceImage [[texture(0)]],sampler sourceSampler [[sampler(1)]])
{
    return sourceImage.sample(sourceSampler,input.uv,level(0));
}
