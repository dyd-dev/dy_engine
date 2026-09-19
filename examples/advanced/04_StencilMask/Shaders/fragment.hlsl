cbuffer Settings:register(b15){float4 value;}
struct Input {float4 position:SV_Position;float2 uv:TEXCOORD0;};
float4 main(Input input):SV_Target{float radius=value.x<.5?.30:.35;if((value.x<.5 || value.z>.5) && length((input.uv-float2(.5,.5))*float2(value.y,1))>radius)discard;
float checker=fmod(floor(input.uv.x*24)+floor(input.uv.y*18),2);return float4(lerp(float3(.1,.35,.9),float3(1,.65,.08),checker),1);}
