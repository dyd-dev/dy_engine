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
        RHI::PrimitiveTopology topology = RHI::PrimitiveTopology::TriangleList;
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
        m_impl->topology = desc.inputAssembly.topology;

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

        if(desc.inputAssembly.vertexBindingCount > 0 || desc.inputAssembly.vertexAttributeCount > 0)
        {
            MTLVertexDescriptor* vertexDesc = [MTLVertexDescriptor vertexDescriptor];
            for(uint32_t bindingIndex = 0; bindingIndex < desc.inputAssembly.vertexBindingCount; ++bindingIndex)
            {
                const RHI::VertexBindingDesc& binding = desc.inputAssembly.vertexBindings[bindingIndex];
                const uint32_t bufferIndex = GetNativeVertexBufferIndex(binding.slot);
                if(bufferIndex >= 31u || binding.stride == 0) return;
                for(uint32_t previous = 0; previous < bindingIndex; ++previous)
                {
                    if(desc.inputAssembly.vertexBindings[previous].slot == binding.slot) return;
                }
                vertexDesc.layouts[bufferIndex].stride = binding.stride;
                vertexDesc.layouts[bufferIndex].stepFunction = binding.inputRate == RHI::VertexInputRate::PerInstance
                    ? MTLVertexStepFunctionPerInstance
                    : MTLVertexStepFunctionPerVertex;
                vertexDesc.layouts[bufferIndex].stepRate = 1;
            }

            for(uint32_t attributeIndex = 0; attributeIndex < desc.inputAssembly.vertexAttributeCount; ++attributeIndex)
            {
                const RHI::VertexAttributeDesc& attribute = desc.inputAssembly.vertexAttributes[attributeIndex];
                const uint32_t bufferIndex = GetNativeVertexBufferIndex(attribute.binding);
                if(attribute.location >= 31u || bufferIndex >= 31u) return;

                bool hasBinding = false;
                for(uint32_t bindingIndex = 0; bindingIndex < desc.inputAssembly.vertexBindingCount; ++bindingIndex)
                {
                    if(desc.inputAssembly.vertexBindings[bindingIndex].slot == attribute.binding)
                    {
                        hasBinding = true;
                        break;
                    }
                }
                if(!hasBinding) return;

                MTLVertexFormat format = MTLVertexFormatInvalid;
                switch(attribute.format)
                {
                case RHI::Format::R32_FLOAT: format = MTLVertexFormatFloat; break;
                case RHI::Format::R32G32_FLOAT: format = MTLVertexFormatFloat2; break;
                case RHI::Format::R32G32B32_FLOAT: format = MTLVertexFormatFloat3; break;
                case RHI::Format::R32G32B32A32_FLOAT: format = MTLVertexFormatFloat4; break;
                case RHI::Format::R8G8B8A8_UNORM: format = MTLVertexFormatUChar4Normalized; break;
                default: return;
                }
                vertexDesc.attributes[attribute.location].format = format;
                vertexDesc.attributes[attribute.location].offset = attribute.offset;
                vertexDesc.attributes[attribute.location].bufferIndex = bufferIndex;
            }
            pipeDesc.vertexDescriptor = vertexDesc;
        }

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

    RHI::PrimitiveTopology MetalPipeline::GetPrimitiveTopology() const
    {
        return m_impl->topology;
    }

    uint32_t MetalPipeline::GetNativeVertexBufferIndex(uint32_t slot)
    {
        return 16u + slot;
    }

    bool MetalPipeline::IsValid() const
    {
        return m_impl->pipelineState != nil;
    }
}
