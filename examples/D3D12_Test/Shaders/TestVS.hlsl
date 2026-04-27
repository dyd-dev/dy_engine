cbuffer TransformDate : register(b0)
{
    float4 offset;
    uint textureIndex;
};

struct VertexInput
{
    float3 Pos : POSITION;
    float2 UV : TEXCOORD;
    float3 Color : COLOR;
};

struct PixelInput
{
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD;
    float3 Color : COLOR;
};

PixelInput main(VertexInput input)
{
    PixelInput output;
    
    float3 finalPos = (input.Pos * 0.05f) + offset.xyz;
    
    output.Pos = float4(finalPos, 1.0f);
    output.UV = input.UV;
    output.Color = input.Color;
    
    // Pass textureIndex using an unused channel or as output if needed?
    // Wait, PS can just read b0 directly as well!
    return output;
}
