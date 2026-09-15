#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform Settings {vec4 value;} settings;
void main(){float bar=step(abs(uv.x-fract(settings.value.x*.35)),.035);float stripe=step(.5,fract(uv.y*40));result=vec4(mix(vec3(.025,.045,.08),vec3(.2+.7*stripe,.8,.3),bar),1);}
