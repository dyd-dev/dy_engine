#version 450
layout(location=0) in vec2 uv;layout(location=0) out vec4 result;
layout(set=0,binding=0) uniform texture2D source;layout(set=0,binding=1) uniform sampler pointSampler;
layout(push_constant) uniform Settings {vec4 value;} settings;
void main(){vec2 coord=vec2(fract(uv.x*2),uv.y);
float d=texture(sampler2D(source,pointSampler),coord).r;float n=settings.value.z,f=settings.value.w;
    float linearDepth=n*f/(f-d*(f-n));bool linearView=uv.x>.5;
    float value=linearView?linearDepth/f:d;result=vec4(vec3(value),1);if(abs(uv.x-.5)<.003)result=vec4(.8,.2,.1,1);}
