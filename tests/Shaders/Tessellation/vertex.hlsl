struct VertexOutput
{
    float2 position : POSITION;
};

VertexOutput main(float2 position : POSITION)
{
    VertexOutput output;
    output.position = position;
    return output;
}
