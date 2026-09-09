cbuffer Transform : register(b15) { float4 transform; };
struct Input { float2 position:TEXCOORD0; float2 uv:TEXCOORD1; float4 color:TEXCOORD2; };
struct Output { float4 position:SV_POSITION; float2 uv:TEXCOORD0; float4 color:TEXCOORD1; };
Output main(Input input) { Output o; o.position=float4(input.position*transform.xy+transform.zw,0,1); o.uv=input.uv; o.color=input.color; return o; }
