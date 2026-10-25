#version 450
struct Params {
    vec2 screenSize;
    float exposure;
    float threshold;
    float bloomStrength;
    float emissiveIntensity;
};
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D image0;
layout(set=0,binding=8) uniform sampler imageSampler;

void main() {

vec3 sum=vec3(0.0);float weightSum=0.0;
for(int i=-6;i<=6;i++) {
    float w=exp(-float(i*i)/12.0);
    sum+=texture(sampler2D(image0,imageSampler),uv+vec2(0.0,float(i)/p.screenSize.y)).rgb*w;weightSum+=w;
}
result0=vec4(sum/weightSum,1.0);

}
