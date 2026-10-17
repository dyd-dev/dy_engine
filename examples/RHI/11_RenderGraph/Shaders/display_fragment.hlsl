Texture2D image:register(t0);
SamplerState imageSampler:register(s1);
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target { return image.Sample(imageSampler,uv); }
