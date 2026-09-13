cbuffer Constants : register(b15) { float4 transform; float4 tint; };
struct Output { float4 position : SV_POSITION; float4 color : TEXCOORD0; };
Output main(float2 position : TEXCOORD0)
{
    Output output;
    output.position = float4(position * transform.xy + transform.zw, 0, 1);
    output.color = tint;
    return output;
}
