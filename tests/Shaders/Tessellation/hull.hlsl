struct ControlPoint
{
    float2 position : POSITION;
};

struct PatchConstants
{
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

PatchConstants PatchMain(InputPatch<ControlPoint, 3> patch, uint patchID : SV_PrimitiveID)
{
    PatchConstants output;
    output.edge[0] = 4.0;
    output.edge[1] = 4.0;
    output.edge[2] = 4.0;
    output.inside = 4.0;
    return output;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_ccw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchMain")]
ControlPoint main(InputPatch<ControlPoint, 3> patch, uint pointID : SV_OutputControlPointID)
{
    return patch[pointID];
}
