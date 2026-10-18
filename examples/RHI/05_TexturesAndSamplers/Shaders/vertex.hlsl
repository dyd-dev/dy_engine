cbuffer Draw:register(b15) { float4 transform; };
struct Output { float4 position:SV_Position; float2 uv:TEXCOORD0; };
Output main(float2 position:POSITION,float2 uv:TEXCOORD0) {
    Output result; result.position=float4(position*transform.zw+transform.xy,0,1); result.uv=uv; return result;
}
