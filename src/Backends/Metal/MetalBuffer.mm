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
	namespace
	{
		[[nodiscard]] bool HasUsage(RHI::BufferUsage value, RHI::BufferUsage usage)
		{
			return (value & usage) != RHI::BufferUsage::None;
		}

		[[nodiscard]] bool IsStateAllowed(
			const RHI::BufferDesc& desc,
			RHI::ResourceState state)
		{
			switch(state)
			{
			case RHI::ResourceState::Undefined:
			case RHI::ResourceState::Common:
			case RHI::ResourceState::CopyDestination:
				return true;
			case RHI::ResourceState::VertexBuffer:
				return HasUsage(desc.usage, RHI::BufferUsage::Vertex);
			case RHI::ResourceState::IndexBuffer:
				return HasUsage(desc.usage, RHI::BufferUsage::Index);
			case RHI::ResourceState::ConstantBuffer:
				return HasUsage(desc.usage, RHI::BufferUsage::Constant);
			case RHI::ResourceState::ShaderResource:
			case RHI::ResourceState::UnorderedAccess:
				return HasUsage(desc.usage, RHI::BufferUsage::Storage);
			default:
				return false;
			}
		}
	}

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
			!IsStateAllowed(desc, desc.initialState))
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
