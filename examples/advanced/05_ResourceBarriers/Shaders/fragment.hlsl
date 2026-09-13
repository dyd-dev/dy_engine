Texture2D<float4> source:register(t0);SamplerState pointSampler:register(s1);
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target{return source.Sample(pointSampler,uv);}
