Texture2D<float> sourceDepth:register(t0);
SamplerState depthSampler:register(s1);
cbuffer Settings:register(b15) {float4 reconstruction;};
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target0
{
    float depth=sourceDepth.SampleLevel(depthSampler,uv,0);
    float4 r=reconstruction;
    float viewDistance=depth>=1 ? r.w : r.z/((1-depth)+depth*(r.z/r.w));
    float2 ray=(uv*float2(2,-2)+float2(-1,1))*r.xy;
    uint bits=asuint(viewDistance*length(float3(ray,1)));
    return float4(bits&255u,(bits>>8)&255u,(bits>>16)&255u,(bits>>24)&255u)/255.0;
}
