//
//  MetalBuffer.mm
//  
//
//  Created by 정준혁 on 4/8/26.
//

#include "MetalBuffer.h"
#import <Metal/Metal.h>

namespace dy::Backends
{
    struct MetalBuffer::Impl
    {
        id<MTLBuffer> buffer = nil;
        uint32_t      size   = 0;
    };

    MetalBuffer::MetalBuffer(const RHI::BufferDesc& desc, void* device)
        : m_impl(new Impl())
    {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
        m_impl->size = desc.size;

        // CPU에서 쓰고 GPU가 읽는 용도 → Shared
        m_impl->buffer = [mtlDevice newBufferWithLength:desc.size
                                                options:MTLResourceStorageModeShared];
    }

    MetalBuffer::~MetalBuffer()
    {
        delete m_impl;
    }

    void* MetalBuffer::Map(uint32_t offset, uint32_t size)
    {
        // Shared 모드라 별도 map 불필요, 그냥 포인터 반환
        return static_cast<uint8_t*>([m_impl->buffer contents]) + offset;
    }

    void MetalBuffer::Unmap()
    {
        // Shared 모드라 별도 unmap 불필요
    }

    uint32_t MetalBuffer::GetSize() const
    {
        return m_impl->size;
    }

    void* MetalBuffer::GetNativeBuffer() const
    {
        return (__bridge void*)m_impl->buffer;
    }
}
