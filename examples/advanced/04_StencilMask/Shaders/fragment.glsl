#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform Settings {vec4 value;} settings;
void main(){float radius=settings.value.x<.5?.30:.35;if((settings.value.x<.5 || settings.value.z>.5) && length((uv-vec2(.5))*vec2(settings.value.y,1))>radius)discard;
float checker=mod(floor(uv.x*24)+floor(uv.y*18),2);result=vec4(mix(vec3(.1,.35,.9),vec3(1,.65,.08),checker),1);}
