#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform texture2D sourceDepth;
layout(set=0,binding=1) uniform sampler depthSampler;
layout(push_constant) uniform Settings {vec4 reconstruction;} settings;
void main()
{
    float depth=textureLod(sampler2D(sourceDepth,depthSampler),uv,0).r;
    vec4 r=settings.reconstruction;
    float viewDistance=depth>=1 ? r.w : r.z/((1-depth)+depth*(r.z/r.w));
    vec2 ray=(uv*vec2(2,-2)+vec2(-1,1))*r.xy;
    uint bits=floatBitsToUint(viewDistance*length(vec3(ray,1)));
    color=vec4(bits&255u,(bits>>8)&255u,(bits>>16)&255u,(bits>>24)&255u)/255.0;
}
