#version 450
struct Params { vec4 sizeTime; vec4 settings; vec4 jitter; vec4 extra; };
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D image0;
#define SAMPLE0(U) texture(sampler2D(image0,imageSampler),U)
layout(set=0,binding=1) uniform texture2D image1;
#define SAMPLE1(U) texture(sampler2D(image1,imageSampler),U)
layout(set=0,binding=2) uniform texture2D image2;
#define SAMPLE2(U) texture(sampler2D(image2,imageSampler),U)
layout(set=0,binding=8) uniform sampler imageSampler;

void main() {

vec4 current=SAMPLE0(uv), motion=SAMPLE1(uv);
vec2 historyUV=uv+motion.xy+(p.jitter.xy-p.jitter.zw)/p.sizeTime.xy;
vec4 history=SAMPLE2(historyUV);
vec3 lower=current.rgb,upper=current.rgb;
for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
    vec3 c=SAMPLE0(uv+vec2(float(x),float(y))/p.sizeTime.xy).rgb;
    lower=min(lower,c);upper=max(upper,c);
}
float weight=p.settings.w;
if(p.extra.x<0.5 || abs(history.a-current.a)>0.1 || any(lessThan(historyUV,vec2(0.0))) || any(greaterThan(historyUV,vec2(1.0)))) weight=0.0;
if(p.sizeTime.w!=2.0) history.rgb=clamp(history.rgb,lower,upper);
if(p.sizeTime.w==1.0) weight=0.0;
result0=vec4(mix(current.rgb,history.rgb,weight),current.a);

}
