cbuffer Constants : register(b15) { float4 color; float4 parameters; };
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Output main(float3 position : TEXCOORD0, float2 uv : TEXCOORD1)
{
    Output output;
    output.position = float4(position.xy * parameters.x, parameters.y, 1);
    output.uv = uv;
    return output;
}
