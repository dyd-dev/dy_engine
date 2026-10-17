struct Output { float4 position:SV_Position; float4 color:COLOR0; };
Output main(float4 position:POSITION,float4 color:COLOR0) { Output result; result.position=position; result.color=color; return result; }
