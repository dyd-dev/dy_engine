#version 450
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
layout(set=0,binding=0) uniform texture2D depthImage;
layout(r32f,set=0,binding=1) uniform readonly image2D previousImage;
layout(r32f,set=0,binding=2) uniform writeonly image2D outputImage;
layout(set=0,binding=8) uniform sampler depthSampler;
layout(push_constant) uniform Operation {uvec4 values;} operation;
void main()
{
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy),destinationSize=imageSize(outputImage);
    if(any(greaterThanEqual(pixel,destinationSize))) return;
    bool first=operation.values.y==0;
    ivec2 sourceSize=first?textureSize(sampler2D(depthImage,depthSampler),0):imageSize(previousImage);
    int stepSize=int(operation.values.x);ivec2 origin=pixel*stepSize;
    float maximum=0;
    for(int y=0;y<stepSize;++y) for(int x=0;x<stepSize;++x)
    {
        ivec2 source=origin+ivec2(x,y);
        if(any(greaterThanEqual(source,sourceSize)))
        {
            // 첫 레벨의 여백은 미피복 영역으로 취급하여 가려졌다고 판단하지 않는다.
            if(first) maximum=1;
            continue;
        }
        float value=first?texelFetch(sampler2D(depthImage,depthSampler),source,0).r:imageLoad(previousImage,source).r;
        if(isnan(value) || isinf(value)) value=1;
        maximum=max(maximum,clamp(value,0,1));
    }
    imageStore(outputImage,pixel,vec4(maximum));
}
