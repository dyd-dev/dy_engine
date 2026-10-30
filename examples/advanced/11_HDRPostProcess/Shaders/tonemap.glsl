#version 450
struct Params { vec4 sizeTime; vec4 settings; vec4 jitter; vec4 extra; };
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D image0;
layout(set=0,binding=1) uniform texture2D image1;
layout(set=0,binding=8) uniform sampler imageSampler;

void main() {

    vec3 hdr=texture(sampler2D(image0,imageSampler),uv).rgb, bloom=texture(sampler2D(image1,imageSampler),uv).rgb;
    hdr+=bloom*p.settings.z;
hdr*=p.settings.x;
    vec3 color=clamp((hdr*(2.51*hdr+0.03))/(hdr*(2.43*hdr+0.59)+0.14),vec3(0.0),vec3(1.0));
    for(int channel=0;channel<3;channel++) {
        float x=max(color[channel],0.0);
        color[channel]=x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055;
    }
    result0=vec4(color,1.0);

}
