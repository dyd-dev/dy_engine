Texture2D<float4> opaqueTexture:register(t5);
SamplerState screenSampler:register(s9);
struct Input {float4 position:SV_Position;float2 uv:TEXCOORD0;};
float4 main(Input input):SV_Target0 {return float4(opaqueTexture.SampleLevel(screenSampler,input.uv,0).rgb,1);}
