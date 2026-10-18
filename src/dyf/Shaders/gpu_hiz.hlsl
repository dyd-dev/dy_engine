Texture2D<float> depthImage:register(t0,space0);
RWTexture2D<float> previousImage:register(u1,space0);
RWTexture2D<float> outputImage:register(u2,space0);
SamplerState depthSampler:register(s8,space0);
cbuffer Operation:register(b15,space0) {uint4 operation;};
[numthreads(8,8,1)]
void main(uint3 threadId:SV_DispatchThreadID)
{
    uint2 pixel=threadId.xy;uint width,height;outputImage.GetDimensions(width,height);
    if(any(pixel>=uint2(width,height))) return;
    bool first=operation.y==0;
    if(first) depthImage.GetDimensions(width,height);else previousImage.GetDimensions(width,height);
    uint stepSize=operation.x;uint2 origin=pixel*stepSize;
    float maximum=0;
    for(uint y=0;y<stepSize;++y) for(uint x=0;x<stepSize;++x)
    {
        uint2 source=origin+uint2(x,y);
        if(any(source>=uint2(width,height)))
        {
            if(first) maximum=1;
            continue;
        }
        float value=first?depthImage.Load(int3(source,0)):previousImage[source];
        if(!isfinite(value)) value=1;
        maximum=max(maximum,saturate(value));
    }
    outputImage[pixel]=maximum;
}
