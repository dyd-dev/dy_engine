struct VertexOut
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void main(
    uint gtid : SV_GroupIndex,
    out vertices VertexOut verts[3],
    out indices uint3 triangles[1])
{
    SetMeshOutputCounts(3, 1);

    if (gtid == 0)
    {
        triangles[0] = uint3(0, 1, 2);
    }

    const float2 positions[3] = {
        float2( 0.0f,  0.5f),
        float2( 0.5f, -0.5f),
        float2(-0.5f, -0.5f)
    };

    const float3 colors[3] = {
        float3(1.0f, 0.0f, 0.0f),
        float3(0.0f, 1.0f, 0.0f),
        float3(0.0f, 0.0f, 1.0f)
    };

    verts[gtid].position = float4(positions[gtid], 0.0f, 1.0f);
    verts[gtid].color = float4(colors[gtid], 1.0f);
}
