cbuffer Transform : register(b15) { float4 transform; };
struct Input { float2 position:TEXCOORD0; float4 color:TEXCOORD1; };
struct Output { float4 position:SV_POSITION; float4 color:TEXCOORD0; };

Output main(Input input)
{
    Output output;
    output.position=float4(input.position*transform.xy+transform.zw,0,1);
    output.color=input.color;
    return output;
}
