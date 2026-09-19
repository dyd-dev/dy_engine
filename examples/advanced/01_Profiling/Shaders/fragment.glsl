#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform Settings {vec4 value;} settings;
void main(){vec2 z=vec2(0),c=(uv-vec2(.65,.5))*2.6;int count=0;
for(int i=0;i<int(settings.value.x);++i){z=vec2(z.x*z.x-z.y*z.y,2*z.x*z.y)+c;if(dot(z,z)>16)break;count++;}
float t=float(count)/max(settings.value.x,1);result=vec4(.08+t,.12+t*t,.2+sqrt(t)*.7,1);}
