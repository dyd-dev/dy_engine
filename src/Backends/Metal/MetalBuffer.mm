//
//  MetalBuffer.mm
//  
//
//  Created by 정준혁 on 4/8/26.
//

#include "MetalBuffer.h"
#include "MetalValidation.h"
#import <Metal/Metal.h>

namespace dyf::Backends
{
    struct MetalBuffer::Impl
    {
        id<MTLBuffer> buffer = nil;
    };

    MetalBuffer::MetalBuffer(const RHI::BufferDesc& desc, void* device)
        : RHI::Buffer(desc)
        , m_impl(new Impl())
		, m_state(desc.initialState)
    {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
		if(mtlDevice == nil || desc.size == 0 || desc.usage == RHI::BufferUsage::None ||
			!MetalValidation::IsBufferStateAllowed(desc, desc.initialState))
			return;
        m_impl->buffer = [mtlDevice newBufferWithLength:desc.size
			options:MTLResourceStorageModePrivate];
    }

    MetalBuffer::~MetalBuffer()
    {
#if !__has_feature(objc_arc)
		[m_impl->buffer release];
#endif
		m_impl->buffer = nil;
        delete m_impl;
    }

	void* MetalBuffer::GetNativeBuffer() const
    {
		return m_impl == nullptr ? nullptr : (__bridge void*)m_impl->buffer;
    }

	RHI::ResourceState MetalBuffer::GetState() const
    {
		return m_state;
    }

	void MetalBuffer::SetState(RHI::ResourceState state) { m_state = state; }
}
