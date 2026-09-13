struct Output {float4 position:SV_Position;float2 uv:TEXCOORD0;};
Output main(uint id:SV_VertexID){Output o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*2-1,0,1);return o;}
