#include <metal_stdlib>
using namespace metal;
struct ToneVertex {float4 position [[position]];float2 uv;};
vertex ToneVertex toneVertex(uint id [[vertex_id]]) {
    ToneVertex o;o.uv=float2((id<<1)&2,id&2);
    o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;
}
fragment float4 toneFragment(ToneVertex in [[stage_in]],texture2d<float> image [[texture(0)]],
    sampler imageSampler [[sampler(1)]],constant float4& settings [[buffer(15)]]) {
    float4 hdr=image.sample(imageSampler,in.uv);
    float3 mapped=max(hdr.rgb*settings.x,0.f);
    mapped=mapped/(mapped+1.f);
    if(settings.y>.5)mapped=pow(mapped,float3(1.f/2.2f));
    return float4(mapped,hdr.a);
}
