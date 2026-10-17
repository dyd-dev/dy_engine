#pragma once
#include <cstdint>
namespace dyf::RHI
{
enum class Feature : uint8_t
{
    Rasterization, Compute, IndirectDraw, IndirectDispatch, DescriptorIndexing,
    TimestampQuery, MeshShader, TaskShader, RayQuery, RayTracingPipeline,
    SamplerLodBias, FractionalDepthBias, Wireframe, DepthBiasClamp, Tessellation,
    IndirectFirstInstance, IndirectTessellation
};

}
