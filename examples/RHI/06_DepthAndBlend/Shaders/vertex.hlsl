cbuffer Draw:register(b15) { float4 transform; };
struct Output { float4 position:SV_Position; float4 color:COLOR0; };
Output main(float3 position:POSITION,float4 color:COLOR0) {
    Output result; result.position=float4(position.xy*transform.zw+transform.xy,position.z,1); result.color=color; return result;
}
