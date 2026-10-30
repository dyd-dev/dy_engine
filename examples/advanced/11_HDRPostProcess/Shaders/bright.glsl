#version 450
struct Params { vec4 sizeTime; vec4 settings; vec4 jitter; vec4 extra; };
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D image0;
#define SAMPLE0(U) texture(sampler2D(image0,imageSampler),U)
layout(set=0,binding=8) uniform sampler imageSampler;

void main() {

// Separable Gaussian: threshold samples during the horizontal pass.
vec3 sum=vec3(0.0); float weightSum=0.0;
for(int i=-6;i<=6;i++) {
    float w=exp(-float(i*i)/12.0); vec3 c=SAMPLE0(uv+vec2(float(i)/p.sizeTime.x,0.0)).rgb;
    float luminance=dot(c,vec3(0.2126,0.7152,0.0722));
    c*=max(luminance-p.settings.y,0.0)/max(luminance,0.001);
    sum+=c*w;weightSum+=w;
}
result0=vec4(sum/weightSum,1.0);

}
