#include <metal_stdlib>
using namespace metal;

struct ControlPoint
{
    float2 position [[attribute(0)]];
};

struct DomainOutput
{
    float4 position [[position]];
};

[[patch(triangle, 3)]]
vertex DomainOutput domainMain(
    patch_control_point<ControlPoint> controlPoints [[stage_in]],
    float3 barycentric [[position_in_patch]])
{
    DomainOutput output;
    const float2 position = controlPoints[0].position * barycentric.x
        + controlPoints[1].position * barycentric.y
        + controlPoints[2].position * barycentric.z;
    output.position = float4(position, 0.0f, 1.0f);
    return output;
}
