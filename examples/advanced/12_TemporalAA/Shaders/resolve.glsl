#version 450
struct Params {
    vec2 screenSize;
    float time;
    float previousTime;
    vec2 jitter;
    vec2 previousJitter;
    float historyWeight;
    float historyValid;
};
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D currentColor;
layout(set=0,binding=1) uniform texture2D motionVectors;
layout(set=0,binding=2) uniform texture2D previousColor;
layout(set=0,binding=8) uniform sampler imageSampler;

void main() {

    vec4 current=texture(sampler2D(currentColor,imageSampler),uv), motion=texture(sampler2D(motionVectors,imageSampler),uv);
vec2 historyUV=uv+motion.xy+(p.jitter-p.previousJitter)/p.screenSize;
    vec4 history=texture(sampler2D(previousColor,imageSampler),historyUV);
vec3 lower=current.rgb,upper=current.rgb;
for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
    vec3 c=texture(sampler2D(currentColor,imageSampler),uv+vec2(float(x),float(y))/p.screenSize).rgb;
    lower=min(lower,c);upper=max(upper,c);
}
float weight=p.historyWeight;
if(p.historyValid<0.5 || abs(history.a-current.a)>0.1 || any(lessThan(historyUV,vec2(0.0))) || any(greaterThan(historyUV,vec2(1.0)))) weight=0.0;
    history.rgb=clamp(history.rgb,lower,upper);
result0=vec4(mix(current.rgb,history.rgb,weight),current.a);

}
