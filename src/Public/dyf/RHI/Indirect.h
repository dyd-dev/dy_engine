#pragma once

#include <cstdint>

namespace dyf::RHI
{
    // GPU-visible argument layouts. Commands consume these structures at 4-byte
    // aligned offsets in a BufferUsage::Indirect buffer. Indexed multidraw uses
    // a 4-byte aligned stride at least sizeof(DrawIndexedIndirectArguments).
    // firstInstance must be zero unless Feature::IndirectFirstInstance is supported.
    // Argument values may be GPU-written; the RHI does not read them back to validate.
    struct DrawIndirectArguments
    {
        uint32_t vertexCount;
        uint32_t instanceCount;
        uint32_t firstVertex;
        uint32_t firstInstance;
    };

    struct DrawIndexedIndirectArguments
    {
        uint32_t indexCount;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t vertexOffset;
        uint32_t firstInstance;
    };

    struct DispatchIndirectArguments
    {
        uint32_t groupCountX;
        uint32_t groupCountY;
        uint32_t groupCountZ;
    };

    static_assert(sizeof(DrawIndirectArguments) == 16);
    static_assert(sizeof(DrawIndexedIndirectArguments) == 20);
    static_assert(sizeof(DispatchIndirectArguments) == 12);
}
