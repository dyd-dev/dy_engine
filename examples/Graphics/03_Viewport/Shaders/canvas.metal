#include <metal_stdlib>
using namespace metal;
struct Input { float2 position [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 color [[attribute(2)]]; };
struct Output { float4 position [[position]]; float2 uv; float4 color; };
vertex Output canvasVertex(Input input [[stage_in]],constant float4& transform [[buffer(15)]])
{ return {float4(input.position*transform.xy+transform.zw,0,1),input.uv,input.color}; }
fragment float4 canvasFragment(Output input [[stage_in]],texture2d<float> image [[texture(0)]],sampler imageSampler [[sampler(1)]])
{ return input.color*image.sample(imageSampler,input.uv); }
