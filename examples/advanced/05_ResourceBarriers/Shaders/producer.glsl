#version 450
layout(location=0) in vec2 uv;layout(location=0) out vec4 result;
layout(push_constant) uniform Settings {vec4 value;} settings;
void main(){if(settings.value.x>.5 && uv.x<.5)discard;
result=vec4(settings.value.x>.5?vec3(.15,.9,.35):vec3(.95,.2,.1),1);}
