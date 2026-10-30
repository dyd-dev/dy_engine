#version 450
layout(location=0) out float copiedDepth;
layout(set=0,binding=5) uniform texture2D sourceDepth;
layout(set=0,binding=9) uniform sampler pointSampler;
void main()
{
    copiedDepth=texelFetch(sampler2D(sourceDepth,pointSampler),ivec2(gl_FragCoord.xy),0).r;
}
