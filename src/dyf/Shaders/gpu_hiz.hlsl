Texture2D<float> depthImage:register(t0,space0);
RWTexture2D<float> previousImage:register(u1,space0);
RWTexture2D<float> outputImage:register(u2,space0);
RWTexture2D<float> secondOutputImage:register(u3,space0);
SamplerState depthSampler:register(s8,space0);
cbuffer Operation:register(b15,space0) {uint4 operation;};
groupshared float localMaximum[64];
[numthreads(8,8,1)]
// Read the original footprint once, then reduce a second mip within each workgroup.
void main(uint3 threadId:SV_DispatchThreadID,uint3 groupId:SV_GroupID,uint3 localId:SV_GroupThreadID)
{
    uint2 pixel=threadId.xy;uint width,height;outputImage.GetDimensions(width,height);
    bool valid=all(pixel<uint2(width,height));
    // 명령 y=0은 원본 깊이에서 시작하는 단계이고, 이후에는 previousImage를 읽는다. x는 한 출력 픽셀이 덮는 입력 폭이다.
    bool first=operation.y==0;
    if(first) depthImage.GetDimensions(width,height);else previousImage.GetDimensions(width,height); // 첫 단계는 원본 깊이 크기, 이후 단계는 직전 Hi-Z 크기를 조회한다.
    uint stepSize=operation.x;uint2 origin=pixel*stepSize;
    // 깊이 범위를 0~1로 제한해 최대값을 누적한다. 비유한값과 첫 단계 가장자리 여백은 1로 두어 보수적으로 미피복 취급한다.
    float maximum=0;
    if(valid) for(uint y=0;y<stepSize;++y) for(uint x=0;x<stepSize;++x)
    {
        uint2 source=origin+uint2(x,y);
        if(any(source>=uint2(width,height))) // 입력 크기 밖의 가장자리 표본은 읽지 않는다.
        {
            if(first) maximum=1; // 첫 단계의 여백은 미피복 깊이 1로 남겨 가장자리의 과도한 가림 제거를 막는다.
            continue;
        }
        float value=first?depthImage.Load(int3(source,0)):previousImage[source];
        if(!isfinite(value)) value=1; // NaN·무한 깊이는 확실한 차폐물로 사용할 수 없으므로 미피복 값 1로 대신한다.
        maximum=max(maximum,saturate(value));
    }
    if(valid) outputImage[pixel]=maximum;
    // Invalid lanes contribute max's identity and still participate in the barrier.
    localMaximum[localId.y*8+localId.x]=maximum;
    GroupMemoryBarrierWithGroupSync();
    if(operation.z==2 && all(localId.xy<uint2(4,4)))
    {
        uint2 secondPixel=groupId.xy*4+localId.xy;
        secondOutputImage.GetDimensions(width,height);
        if(all(secondPixel<uint2(width,height)))
        {
            uint child=localId.y*16+localId.x*2;
            secondOutputImage[secondPixel]=max(max(localMaximum[child],localMaximum[child+1]),
                max(localMaximum[child+8],localMaximum[child+9]));
        }
    }
}
