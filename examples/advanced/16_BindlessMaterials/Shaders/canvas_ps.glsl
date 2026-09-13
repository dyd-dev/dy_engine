#version 450
layout(set = 0, binding = 0) uniform texture2D materialTexture;
layout(set = 0, binding = 4) uniform sampler materialSampler;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(sampler2D(materialTexture, materialSampler), uv); }
