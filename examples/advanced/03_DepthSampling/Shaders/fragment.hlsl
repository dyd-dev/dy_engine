Texture2D<float> source:register(t0);SamplerState pointSampler:register(s1);
cbuffer Settings:register(b15){float4 value;}
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target{
bool split=value.x<.5;float2 coord=float2(split?frac(uv.x*2):uv.x,uv.y);float d=source.Sample(pointSampler,coord);
float n=value.z,f=value.w,linearDepth=n*f/max(f-d*(f-n),.000001);bool linearView=value.x>1.5 || (split && uv.x>.5);
float intensity=linearView?linearDepth/f:d;if(split && abs(uv.x-.5)<.003)return float4(.8,.2,.1,1);return float4(intensity,intensity,intensity,1);}
