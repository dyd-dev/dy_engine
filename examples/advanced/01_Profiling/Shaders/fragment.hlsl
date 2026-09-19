cbuffer Settings:register(b15){float4 value;}
struct Input {float4 position:SV_Position;float2 uv:TEXCOORD0;};
float4 main(Input input):SV_Target{float2 z=0,c=(input.uv-float2(.65,.5))*2.6;int count=0;
[loop]for(int i=0;i<int(value.x);++i){z=float2(z.x*z.x-z.y*z.y,2*z.x*z.y)+c;if(dot(z,z)>16)break;count++;}
float t=float(count)/max(value.x,1);return float4(.08+t,.12+t*t,.2+sqrt(t)*.7,1);}
