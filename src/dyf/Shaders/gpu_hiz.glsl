#version 450
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
layout(set=0,binding=0) uniform texture2D depthImage;
layout(r32f,set=0,binding=1) uniform readonly image2D previousImage;
layout(r32f,set=0,binding=2) uniform writeonly image2D outputImage;
layout(set=0,binding=8) uniform sampler depthSampler;
layout(push_constant) uniform Operation {uvec4 values;} operation;
// 출력 픽셀 하나를 담당한다. 입력 영역을 정수 좌표로 읽어 최대값을 누적하므로 보간이나 평균 깊이를 만들지 않는다.
void main()
{
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy),destinationSize=imageSize(outputImage);
    if(any(greaterThanEqual(pixel,destinationSize))) return; // 8의 배수로 올림된 dispatch에서 출력 크기를 벗어난 스레드는 종료한다.
    // 명령 y=0은 원본 깊이에서 시작하는 단계이고, 이후에는 previousImage를 읽는다. x는 한 출력 픽셀이 덮는 입력 폭이다.
    bool first=operation.values.y==0;
    ivec2 sourceSize=first?textureSize(sampler2D(depthImage,depthSampler),0):imageSize(previousImage);
    int stepSize=int(operation.values.x);ivec2 origin=pixel*stepSize;
    // 깊이 범위를 0~1로 제한해 최대값을 누적한다. 비유한값과 첫 단계 가장자리 여백은 1로 두어 보수적으로 미피복 취급한다.
    float maximum=0;
    for(int y=0;y<stepSize;++y) for(int x=0;x<stepSize;++x)
    {
        ivec2 source=origin+ivec2(x,y);
        if(any(greaterThanEqual(source,sourceSize))) // 입력 크기 밖의 가장자리 표본은 읽지 않는다.
        {
            if(first) maximum=1; // 첫 단계의 여백은 미피복 깊이 1로 남겨 가장자리의 과도한 가림 제거를 막는다.
            continue;
        }
        float value=first?texelFetch(sampler2D(depthImage,depthSampler),source,0).r:imageLoad(previousImage,source).r;
        if(isnan(value) || isinf(value)) value=1; // NaN·무한 깊이는 확실한 차폐물로 사용할 수 없으므로 미피복 값 1로 대신한다.
        maximum=max(maximum,clamp(value,0,1));
    }
    imageStore(outputImage,pixel,vec4(maximum));
}
