#version 450

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D gTexture;

void main()
{
    vec4 sampledColor = texture(gTexture, fragUv);
    vec2 centeredUv = fragUv * 2.0 - 1.0;
    float vignette = 1.0 - dot(centeredUv, centeredUv) * 0.18;
    outColor = sampledColor * vec4(fragColor.rgb * clamp(vignette, 0.75, 1.0), fragColor.a);
}
