#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform texture2D sourceImage;
layout(set=0,binding=1) uniform sampler sourceSampler;
// 전체 화면 UV로 원본의 mip 0을 읽어 출력한다. 여기서는 RGB와 알파를 별도로 수정하지 않는다.
void main()
{
    color=textureLod(sampler2D(sourceImage,sourceSampler),uv,0);
}
