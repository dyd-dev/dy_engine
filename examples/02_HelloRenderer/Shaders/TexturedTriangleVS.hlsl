struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0f);
    output.uv = input.uv;
    return output;
}
