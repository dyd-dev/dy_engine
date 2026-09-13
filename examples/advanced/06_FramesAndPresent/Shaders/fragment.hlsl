cbuffer Settings:register(b15){float4 value;}
struct Input {float4 position:SV_Position;float2 uv:TEXCOORD0;};
float4 main(Input input):SV_Target{float bar=step(abs(input.uv.x-frac(value.x*.35)),.035);float stripe=step(.5,frac(input.uv.y*40));return float4(lerp(float3(.025,.045,.08),float3(.2+.7*stripe,.8,.3),bar),1);}
