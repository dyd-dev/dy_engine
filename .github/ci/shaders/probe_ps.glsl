#version 450
layout(set = 0, binding = 0) uniform texture2D colorTexture;
layout(set = 0, binding = 1) uniform sampler colorSampler;
layout(push_constant) uniform Constants { vec4 color; vec4 parameters; } constants;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main()
{
    outColor = constants.parameters.w > 0.5 ? texture(sampler2D(colorTexture, colorSampler), uv) : constants.color;
}
