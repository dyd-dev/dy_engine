cbuffer TransformDate : register(b0)
{
    float4 offset;
};

struct VertexInput
{
    float3 Pos : POSITION;
    float2 UV : TEXCOORD;
};

struct PixelInput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD;
};

PixelInput main(VertexInput input)
{
    PixelInput output;
    
    float3 finalPos = (input.Pos * 0.05f) + offset.xyz;
    
    output.Pos = float4(finalPos, 1.0f);
    output.UV = input.UV;
    
    return output;
}
