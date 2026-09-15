#version 450
layout(set=0,binding=0) uniform texture2D image;
layout(set=0,binding=1) uniform sampler imageSampler;
layout(push_constant) uniform Transform { vec4 value; vec4 settings; } transform;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 color;
layout(location=0) out vec4 result;
// 이미지는 선형으로 샘플링하고 Canvas의 화면 색상 공간에서 tint를 곱한다.
float ToSrgb(float value)
{
    return value<=0.0031308 ? value*12.92 : 1.055*pow(value,1.0/2.4)-0.055;
}
float ToLinear(float value)
{
    return value<=0.04045 ? value/12.92 : pow((value+0.055)/1.055,2.4);
}

void main()
{
    vec4 sampled=texture(sampler2D(image,imageSampler),uv);
    sampled.rgb=vec3(ToSrgb(sampled.r),ToSrgb(sampled.g),ToSrgb(sampled.b));
    result=color*sampled;
    if(transform.settings.x>0.5)
        result.rgb=vec3(ToLinear(result.r),ToLinear(result.g),ToLinear(result.b));
}
