#include "MetalShader.h"

#include <dispatch/dispatch.h>
#import <Metal/Metal.h>

namespace dy::Backends
{
	struct MetalShader::Impl
	{
		id<MTLLibrary> library = nil;
		id<MTLFunction> function = nil;
	};

	MetalShader::MetalShader(const RHI::ShaderDesc& desc, void* device)
		: RHI::Shader(desc)
		, m_impl(new Impl())
	{
		id<MTLDevice> metalDevice = (__bridge id<MTLDevice>)device;
		if(metalDevice == nil || desc.binary == nullptr || desc.binarySize == 0 ||
			GetStage() == RHI::ShaderStage::Unknown || GetEntryPoint()[0] == '\0' ||
			GetBinary() == nullptr || GetBinarySize() == 0)
		{
			return;
		}

		dispatch_data_t libraryData = dispatch_data_create(
			GetBinary(),
			GetBinarySize(),
			dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
			DISPATCH_DATA_DESTRUCTOR_NONE);
		if(libraryData == nullptr) return;

		NSError* error = nil;
		m_impl->library = [metalDevice newLibraryWithData:libraryData error:&error];
#if !OS_OBJECT_USE_OBJC
		dispatch_release(libraryData);
#endif
		if(m_impl->library == nil) return;

		NSString* entryPoint = [NSString stringWithUTF8String:GetEntryPoint()];
		if(entryPoint == nil) return;
		m_impl->function = [m_impl->library newFunctionWithName:entryPoint];
	}

	MetalShader::~MetalShader()
	{
		if(m_impl == nullptr) return;
#if !__has_feature(objc_arc)
		[m_impl->function release];
		[m_impl->library release];
#endif
		m_impl->function = nil;
		m_impl->library = nil;
		delete m_impl;
	}

	void* MetalShader::GetNativeFunction() const
	{
		return m_impl == nullptr ? nullptr : (__bridge void*)m_impl->function;
	}
}
