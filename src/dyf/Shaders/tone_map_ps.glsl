#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform texture2D hdrImage;
layout(set=0,binding=1) uniform sampler hdrSampler;
layout(push_constant) uniform Settings {vec4 values;} settings;
void main() {
    vec4 hdr=texture(sampler2D(hdrImage,hdrSampler),uv);
    vec3 mapped=max(hdr.rgb*settings.values.x,vec3(0));
    mapped=mapped/(mapped+vec3(1));
    if(settings.values.y>.5)mapped=pow(mapped,vec3(1./2.2));
    color=vec4(mapped,hdr.a);
}
