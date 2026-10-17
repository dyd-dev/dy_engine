#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=5) uniform texture2D opaqueTexture;
layout(set=0,binding=9) uniform sampler screenSampler;
void main(){color=vec4(textureLod(sampler2D(opaqueTexture,screenSampler),uv,0).rgb,1);}
