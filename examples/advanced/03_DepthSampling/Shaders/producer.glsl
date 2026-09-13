#version 450
layout(location=0) in vec2 uv;layout(push_constant) uniform Settings {vec4 value;} settings;
void main(){float n=settings.value.z,f=settings.value.w;
float distance=mix(n+1.0,f*.9,uv.x);if(length(uv-vec2(.3,.5))<.2)distance=n+2.0;
gl_FragDepth=(f-f*n/distance)/(f-n);}
