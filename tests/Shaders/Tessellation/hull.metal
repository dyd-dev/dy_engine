#include <metal_stdlib>
using namespace metal;

struct TriangleTessellationFactors
{
    half edge[3];
    half inside;
};

kernel void hullMain(
    uint patchInstance [[thread_position_in_grid]],
    device TriangleTessellationFactors* factors [[buffer(30)]])
{
    factors[patchInstance].edge[0] = half(4.0h);
    factors[patchInstance].edge[1] = half(4.0h);
    factors[patchInstance].edge[2] = half(4.0h);
    factors[patchInstance].inside = half(4.0h);
}
