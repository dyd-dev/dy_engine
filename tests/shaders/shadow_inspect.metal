#include <metal_stdlib>
using namespace metal;
struct Instance {float4x4 model,previous;};
kernel void shadowInspect(device const uint* words [[buffer(0)]],texture2d<float,access::write> outputImage [[texture(1)]],
    device const Instance* instances [[buffer(2)]],constant uint4& counts [[buffer(15)]],uint id [[thread_position_in_grid]]) {
    if(id>=counts.x)return;
    uint value;
    if(counts.y==0)value=words[id];
    else {
        uint row=id/32,component=id%32,k=component%16;
        float4x4 m=component<16?instances[row].model:instances[row].previous;
        value=as_type<uint>(m[k/4][k%4]);
    }
    outputImage.write(float4(value&255u,(value>>8u)&255u,(value>>16u)&255u,value>>24u)/255.0,uint2(id,0));
}
