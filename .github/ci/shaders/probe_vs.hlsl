cbuffer Constants : register(b0) { float4 color; float4 parameters; };
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Output main(float3 position : POSITION, float2 uv : TEXCOORD0)
{
    Output output;
    output.position = float4(position.x * parameters.x, position.y * parameters.x * parameters.z, parameters.y, 1);
    output.uv = uv;
    return output;
}
