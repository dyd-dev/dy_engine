#include <metal_stdlib>
using namespace metal;
// Read the original footprint once, then reduce a second mip within each workgroup.
kernel void kernelMain(uint3 threadId [[thread_position_in_grid]],
    uint3 groupId [[threadgroup_position_in_grid]],uint3 localId [[thread_position_in_threadgroup]],
    texture2d<float> depthImage [[texture(0)]],texture2d<float,access::read_write> previousImage [[texture(1)]],
    texture2d<float,access::read_write> outputImage [[texture(2)]],
    texture2d<float,access::read_write> secondOutputImage [[texture(3)]],sampler depthSampler [[sampler(8)]],
    constant uint4& operation [[buffer(15)]])
{
    uint2 pixel=threadId.xy;
    bool valid=all(pixel<uint2(outputImage.get_width(),outputImage.get_height()));
    // 명령 y=0은 원본 깊이에서 시작하는 단계이고, 이후에는 previousImage를 읽는다. x는 한 출력 픽셀이 덮는 입력 폭이다.
    bool first=operation.y==0;
    uint2 sourceSize=first?uint2(depthImage.get_width(),depthImage.get_height()):uint2(previousImage.get_width(),previousImage.get_height());
    uint stepSize=operation.x;uint2 origin=pixel*stepSize;
    // 깊이 범위를 0~1로 제한해 최대값을 누적한다. 비유한값과 첫 단계 가장자리 여백은 1로 두어 보수적으로 미피복 취급한다.
    float maximum=0;
    if(valid) for(uint y=0;y<stepSize;++y) for(uint x=0;x<stepSize;++x)
    {
        uint2 source=origin+uint2(x,y);
        if(any(source>=sourceSize)) // 입력 크기 밖의 가장자리 표본은 읽지 않는다.
        {
            if(first) maximum=1; // 첫 단계의 여백은 미피복 깊이 1로 남겨 가장자리의 과도한 가림 제거를 막는다.
            continue;
        }
        float value=first?depthImage.read(source).r:previousImage.read(source).r;
        if(!isfinite(value)) value=1; // NaN·무한 깊이는 확실한 차폐물로 사용할 수 없으므로 미피복 값 1로 대신한다.
        maximum=max(maximum,clamp(value,0.f,1.f));
    }
    if(valid) outputImage.write(float4(maximum),pixel);
    // Invalid lanes contribute max's identity and still participate in the barrier.
    threadgroup float localMaximum[64];
    localMaximum[localId.y*8+localId.x]=maximum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(operation.z==2 && all(localId.xy<uint2(4)))
    {
        uint2 secondPixel=groupId.xy*4+localId.xy;
        if(all(secondPixel<uint2(secondOutputImage.get_width(),secondOutputImage.get_height())))
        {
            uint child=localId.y*16+localId.x*2;
            float reduced=max(max(localMaximum[child],localMaximum[child+1]),
                max(localMaximum[child+8],localMaximum[child+9]));
            secondOutputImage.write(float4(reduced),secondPixel);
        }
    }
}
