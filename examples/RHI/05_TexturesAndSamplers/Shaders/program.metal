#include <metal_stdlib>
using namespace metal;
struct Input { float2 position [[attribute(0)]]; float2 uv [[attribute(1)]]; };
struct Output { float4 position [[position]]; float2 uv; };
vertex Output vertexMain(Input input [[stage_in]],constant float4& transform [[buffer(15)]]) {
    return {float4(input.position*transform.zw+transform.xy,0,1),input.uv};
}
fragment float4 fragmentMain(Output input [[stage_in]],texture2d<float> image [[texture(0)]],sampler imageSampler [[sampler(1)]]) {
    return image.sample(imageSampler,input.uv);
}
