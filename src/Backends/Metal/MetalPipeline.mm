#include "MetalPipeline.h"
#import <Metal/Metal.h>

namespace dy::Backends
{
    struct MetalShader::Impl
    {
        id<MTLLibrary> library = nil;
        id<MTLFunction> function = nil;
        RHI::ShaderStage stage = RHI::ShaderStage::Unknown;
    };

    MetalShader::MetalShader(const RHI::ShaderDesc& desc, void* device)
        : m_impl(new Impl())
    {
        m_impl->stage = desc.stage;

        dispatch_data_t libraryData = dispatch_data_create(
            desc.binary,
            desc.binarySize,
            dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
            DISPATCH_DATA_DESTRUCTOR_NONE);
        if(libraryData == nullptr) return;

        NSError* error = nil;
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
        m_impl->library = [mtlDevice newLibraryWithData:libraryData error:&error];
#if !OS_OBJECT_USE_OBJC
        dispatch_release(libraryData);
#endif
        if(m_impl->library == nil)
        {
            NSLog(@"Metal library load failed: %@", error);
            return;
        }

        NSString* entryPoint = [NSString stringWithUTF8String:desc.entryPoint];
        m_impl->function = [m_impl->library newFunctionWithName:entryPoint];
        if(m_impl->function == nil) NSLog(@"Metal shader entry point not found: %@", entryPoint);
    }

    RHI::ShaderStage MetalShader::GetStage() const
    {
        return m_impl->stage;
    }

    MetalShader::~MetalShader()
    {
#if !__has_feature(objc_arc)
        [m_impl->function release];
        [m_impl->library release];
#endif
        delete m_impl;
    }

    void* MetalShader::GetNativeFunction() const
    {
        return (__bridge void*)m_impl->function;
    }

    bool MetalShader::IsValid() const
    {
        return m_impl->function != nil;
    }

    struct MetalPipeline::Impl
    {
        id<MTLRenderPipelineState> pipelineState    = nil;
        id<MTLDepthStencilState>   depthStencilState = nil;
        float depthBias = 0.0f;
        float depthBiasSlope = 0.0f;
        float depthBiasClamp = 0.0f;
    };

    static MTLPixelFormat ToMTLFormat(RHI::Format format)
    {
        switch(format)
        {
            case RHI::Format::R8G8B8A8_UNORM:        return MTLPixelFormatRGBA8Unorm;
            case RHI::Format::B8G8R8A8_UNORM:        return MTLPixelFormatBGRA8Unorm;
            case RHI::Format::R8G8B8A8_UNORM_SRGB:   return MTLPixelFormatRGBA8Unorm_sRGB;
            case RHI::Format::B8G8R8A8_UNORM_SRGB:   return MTLPixelFormatBGRA8Unorm_sRGB;
            case RHI::Format::R16G16B16A16_FLOAT:    return MTLPixelFormatRGBA16Float;
            case RHI::Format::R32G32B32A32_FLOAT:    return MTLPixelFormatRGBA32Float;
            case RHI::Format::D32_FLOAT:             return MTLPixelFormatDepth32Float;
            case RHI::Format::D24_UNORM_S8_UINT:     return MTLPixelFormatDepth24Unorm_Stencil8;
            default:                                 return MTLPixelFormatInvalid;
        }
    }

    MetalPipeline::MetalPipeline(const RHI::GraphicsPipelineDesc& desc, void* device)
        : m_impl(new Impl())
    {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
        NSError* error = nil;
        m_impl->depthBias = static_cast<float>(desc.depthBias);
        m_impl->depthBiasSlope = desc.depthBiasSlope;
        m_impl->depthBiasClamp = desc.depthBiasClamp;

        const auto* vertexShader = dynamic_cast<const MetalShader*>(desc.vertexShader);
        const auto* fragmentShader = dynamic_cast<const MetalShader*>(desc.fragmentShader);
        if(vertexShader == nullptr || vertexShader->GetStage() != RHI::ShaderStage::Vertex) return;
        const bool hasFragmentShader = desc.fragmentShader != nullptr;
        if(hasFragmentShader &&
           (fragmentShader == nullptr || fragmentShader->GetStage() != RHI::ShaderStage::Fragment)) return;

        id<MTLFunction> vertFunc = (__bridge id<MTLFunction>)vertexShader->GetNativeFunction();
        id<MTLFunction> fragFunc = hasFragmentShader
            ? (__bridge id<MTLFunction>)fragmentShader->GetNativeFunction()
            : nil;

        MTLRenderPipelineDescriptor* pipeDesc = [MTLRenderPipelineDescriptor new];
        pipeDesc.vertexFunction   = vertFunc;
        pipeDesc.fragmentFunction = fragFunc;

        if(desc.renderTargetFormat != RHI::Format::Unknown)
            pipeDesc.colorAttachments[0].pixelFormat = ToMTLFormat(desc.renderTargetFormat);

        if(desc.depthStencilFormat != RHI::Format::Unknown)
            pipeDesc.depthAttachmentPixelFormat = ToMTLFormat(desc.depthStencilFormat);

        m_impl->pipelineState = [mtlDevice newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
        if(!m_impl->pipelineState) { NSLog(@"파이프라인 생성 실패: %@", error); return; }

        // DepthStencil 상태 생성
        if(desc.depthEnable)
        {
            MTLDepthStencilDescriptor* depthDesc = [MTLDepthStencilDescriptor new];
            depthDesc.depthCompareFunction = MTLCompareFunctionLess;
            depthDesc.depthWriteEnabled    = YES;
            m_impl->depthStencilState = [mtlDevice newDepthStencilStateWithDescriptor:depthDesc];
        }
    }

    MetalPipeline::~MetalPipeline()
    {
        delete m_impl;
    }

    void* MetalPipeline::GetNativePipeline() const
    {
        return (__bridge void*)m_impl->pipelineState;
    }

    void* MetalPipeline::GetNativeDepthStencil() const
    {
        return (__bridge void*)m_impl->depthStencilState;
    }

    float MetalPipeline::GetDepthBias() const
    {
        return m_impl->depthBias;
    }

    float MetalPipeline::GetDepthBiasSlope() const
    {
        return m_impl->depthBiasSlope;
    }

    float MetalPipeline::GetDepthBiasClamp() const
    {
        return m_impl->depthBiasClamp;
    }

    bool MetalPipeline::IsValid() const
    {
        return m_impl->pipelineState != nil;
    }
}
