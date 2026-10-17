struct Input { float2 position:POSITION; float4 color:COLOR0; float4 transform:TEXCOORD0; };
struct Output { float4 position:SV_Position; float4 color:COLOR0; };
Output main(Input input) {
    Output result; result.position=float4(input.position*input.transform.zw+input.transform.xy,0,1);
    result.color=input.color; return result;
}
