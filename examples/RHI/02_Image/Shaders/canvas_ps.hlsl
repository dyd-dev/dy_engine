Texture2D image:register(t0);
SamplerState imageSampler:register(s1);
float4 main(float4 position:SV_POSITION,float2 uv:TEXCOORD0,float4 color:TEXCOORD1):SV_TARGET { return color*image.Sample(imageSampler,uv); }
