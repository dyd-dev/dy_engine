cbuffer Settings:register(b15){float4 value;}
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target{if(value.x>.5 && uv.x<.5)discard;return float4(value.x>.5?float3(.15,.9,.35):float3(.95,.2,.1),1);}
