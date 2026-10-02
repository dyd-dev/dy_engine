#pragma once
#include <cstdint>
namespace dyf::RHI
{
enum class Feature : uint8_t
{
    Rasterization, Compute, IndirectDraw, IndirectDispatch, DescriptorIndexing,
    TimestampQuery, MeshShader, TaskShader, RayQuery, RayTracingPipeline,
    SamplerLodBias, FractionalDepthBias, Wireframe, DepthBiasClamp, Tessellation
};

// 장치가 실제로 제공하는 한도다. 바이트 단위와 정렬 조건은 값 이름에 따른다.
enum class Limit : uint8_t
{
    InlineConstantBytes, Texture2DDimension,
    UniformBufferOffsetAlignment, StorageBufferOffsetAlignment,
    UniformBufferBytes, StorageBufferBytes, SamplerAnisotropy, TessellationPatchControlPoints
};
}
