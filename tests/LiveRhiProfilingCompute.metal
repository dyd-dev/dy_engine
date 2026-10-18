#include <metal_stdlib>
using namespace metal;
kernel void liveCompute(uint3 id [[thread_position_in_grid]]) {}
