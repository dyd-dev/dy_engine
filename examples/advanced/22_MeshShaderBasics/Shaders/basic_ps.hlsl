struct VertexOut
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

float4 main(VertexOut pin) : SV_Target
{
    return pin.color;
}
