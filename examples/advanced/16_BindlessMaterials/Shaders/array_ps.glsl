#version 450
layout(set = 0, binding = 0) uniform texture2D materialTextures[4];
layout(set = 0, binding = 4) uniform sampler materialSampler;
layout(push_constant) uniform Constants { vec4 transform; vec4 parameters; } constants;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main()
{
    uint materialIndex = uint(constants.parameters.x);
    outColor = texture(sampler2D(materialTextures[materialIndex], materialSampler), uv);
}
