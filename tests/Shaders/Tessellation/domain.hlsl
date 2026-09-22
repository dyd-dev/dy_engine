struct ControlPoint
{
    float2 position : POSITION;
};

struct PatchConstants
{
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

[domain("tri")]
float4 main(
    PatchConstants constants,
    const OutputPatch<ControlPoint, 3> patch,
    float3 barycentric : SV_DomainLocation) : SV_Position
{
    const float2 position = patch[0].position * barycentric.x
        + patch[1].position * barycentric.y
        + patch[2].position * barycentric.z;
    return float4(position, 0.0, 1.0);
}
