#include <metal_stdlib>
using namespace metal;
kernel void kernelMain(uint3 threadId [[thread_position_in_grid]],
    texture2d<float> depthImage [[texture(0)]],texture2d<float,access::read_write> previousImage [[texture(1)]],
    texture2d<float,access::read_write> outputImage [[texture(2)]],sampler depthSampler [[sampler(8)]],
    constant uint4& operation [[buffer(15)]])
{
    uint2 pixel=threadId.xy;
    if(any(pixel>=uint2(outputImage.get_width(),outputImage.get_height()))) return;
    bool first=operation.y==0;
    uint2 sourceSize=first?uint2(depthImage.get_width(),depthImage.get_height()):uint2(previousImage.get_width(),previousImage.get_height());
    uint stepSize=operation.x;uint2 origin=pixel*stepSize;
    float maximum=0;
    for(uint y=0;y<stepSize;++y) for(uint x=0;x<stepSize;++x)
    {
        uint2 source=origin+uint2(x,y);
        if(any(source>=sourceSize))
        {
            if(first) maximum=1;
            continue;
        }
        float value=first?depthImage.read(source).r:previousImage.read(source).r;
        if(!isfinite(value)) value=1;
        maximum=max(maximum,clamp(value,0.f,1.f));
    }
    outputImage.write(float4(maximum),pixel);
}
