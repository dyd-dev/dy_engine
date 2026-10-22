#include <metal_stdlib>
using namespace metal;
// 출력 픽셀 하나를 담당한다. 입력 영역을 정수 좌표로 읽어 최대값을 누적하므로 보간이나 평균 깊이를 만들지 않는다.
kernel void kernelMain(uint3 threadId [[thread_position_in_grid]],
    texture2d<float> depthImage [[texture(0)]],texture2d<float,access::read_write> previousImage [[texture(1)]],
    texture2d<float,access::read_write> outputImage [[texture(2)]],sampler depthSampler [[sampler(8)]],
    constant uint4& operation [[buffer(15)]])
{
    uint2 pixel=threadId.xy;
    if(any(pixel>=uint2(outputImage.get_width(),outputImage.get_height()))) return; // 8의 배수로 올림된 dispatch에서 출력 크기를 벗어난 스레드는 종료한다.
    // 명령 y=0은 원본 깊이에서 시작하는 단계이고, 이후에는 previousImage를 읽는다. x는 한 출력 픽셀이 덮는 입력 폭이다.
    bool first=operation.y==0;
    uint2 sourceSize=first?uint2(depthImage.get_width(),depthImage.get_height()):uint2(previousImage.get_width(),previousImage.get_height());
    uint stepSize=operation.x;uint2 origin=pixel*stepSize;
    // 깊이 범위를 0~1로 제한해 최대값을 누적한다. 비유한값과 첫 단계 가장자리 여백은 1로 두어 보수적으로 미피복 취급한다.
    float maximum=0;
    for(uint y=0;y<stepSize;++y) for(uint x=0;x<stepSize;++x)
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
    outputImage.write(float4(maximum),pixel);
}
