#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D image0;
layout(set=0,binding=8) uniform sampler imageSampler;

void main() {

    vec3 color=texture(sampler2D(image0,imageSampler),uv).rgb;
    for(int channel=0;channel<3;channel++) {
        float x=max(color[channel],0.0);
        color[channel]=x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055;
    }
    result0=vec4(color,1.0);

}
