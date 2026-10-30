#version 450
struct Params { vec4 sizeTime; vec4 settings; vec4 jitter; vec4 extra; };
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D image0;
#define SAMPLE0(U) texture(sampler2D(image0,imageSampler),U)
layout(set=0,binding=1) uniform texture2D image1;
#define SAMPLE1(U) texture(sampler2D(image1,imageSampler),U)
layout(set=0,binding=8) uniform sampler imageSampler;

float SrgbChannel(float x) { x=max(x,0.0); return x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055; }
vec3 srgb(vec3 c) { return vec3(SrgbChannel(c.r),SrgbChannel(c.g),SrgbChannel(c.b)); }
vec3 aces(vec3 c) { return clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),vec3(0.0),vec3(1.0)); }

void main() {

vec3 hdr=SAMPLE0(uv).rgb, bloom=SAMPLE1(uv).rgb;
if(p.sizeTime.w!=3.0) hdr+=bloom*p.settings.z;
hdr*=p.settings.x;
vec3 color=aces(hdr);
if(p.sizeTime.w==1.0) color=hdr/(1.0+hdr);
if(p.sizeTime.w==2.0) color=clamp(hdr,vec3(0.0),vec3(1.0));
if(p.sizeTime.w==4.0) color=aces(bloom*p.settings.x);
result0=vec4(srgb(color),1.0);

}
