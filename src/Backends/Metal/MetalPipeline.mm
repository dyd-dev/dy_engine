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
        RHI::RasterizationDesc rasterization = {};
        uint32_t stencilReference = 0;
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
        m_impl->rasterization = desc.rasterization;
        m_impl->stencilReference = desc.depthStencil.stencilReference;
        m_impl->topology = desc.inputAssembly.topology;
        if((desc.rasterization.fillMode != RHI::FillMode::Solid && desc.rasterization.fillMode != RHI::FillMode::Wireframe) ||
           (desc.rasterization.cullMode != RHI::CullMode::None && desc.rasterization.cullMode != RHI::CullMode::Front && desc.rasterization.cullMode != RHI::CullMode::Back) ||
           (desc.rasterization.frontFace != RHI::FrontFace::CounterClockwise && desc.rasterization.frontFace != RHI::FrontFace::Clockwise) ||
           static_cast<uint32_t>(desc.depthStencil.depthCompareOp) > static_cast<uint32_t>(RHI::CompareOp::Always)) return;

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

        for(uint32_t attachmentIndex = 0; attachmentIndex < desc.colorAttachmentCount; ++attachmentIndex)
        {
            const RHI::ColorAttachmentDesc& source = desc.colorAttachments[attachmentIndex];
            if(source.format == RHI::Format::Unknown || (source.writeMask & ~RHI::ColorWriteAll) != 0) return;
            switch(source.format)
            {
            case RHI::Format::R8G8B8A8_UNORM:
            case RHI::Format::B8G8R8A8_UNORM:
            case RHI::Format::R8G8B8A8_UNORM_SRGB:
            case RHI::Format::B8G8R8A8_UNORM_SRGB:
            case RHI::Format::R16G16B16A16_FLOAT:
            case RHI::Format::R32G32B32A32_FLOAT:
                break;
            default:
                return;
            }
            MTLRenderPipelineColorAttachmentDescriptor* target = pipeDesc.colorAttachments[attachmentIndex];
            target.pixelFormat = ToMTLFormat(source.format);
            if(target.pixelFormat == MTLPixelFormatInvalid) return;
            target.blendingEnabled = source.blendEnable ? YES : NO;
            const RHI::BlendFactor factors[] = {
                source.sourceColorFactor,
                source.destinationColorFactor,
                source.sourceAlphaFactor,
                source.destinationAlphaFactor
            };
            MTLBlendFactor nativeFactors[4] = {};
            for(uint32_t factorIndex = 0; factorIndex < 4; ++factorIndex)
            {
                switch(factors[factorIndex])
                {
                case RHI::BlendFactor::Zero: nativeFactors[factorIndex] = MTLBlendFactorZero; break;
                case RHI::BlendFactor::One: nativeFactors[factorIndex] = MTLBlendFactorOne; break;
                case RHI::BlendFactor::SourceColor: nativeFactors[factorIndex] = MTLBlendFactorSourceColor; break;
                case RHI::BlendFactor::OneMinusSourceColor: nativeFactors[factorIndex] = MTLBlendFactorOneMinusSourceColor; break;
                case RHI::BlendFactor::SourceAlpha: nativeFactors[factorIndex] = MTLBlendFactorSourceAlpha; break;
                case RHI::BlendFactor::OneMinusSourceAlpha: nativeFactors[factorIndex] = MTLBlendFactorOneMinusSourceAlpha; break;
                case RHI::BlendFactor::DestinationColor: nativeFactors[factorIndex] = MTLBlendFactorDestinationColor; break;
                case RHI::BlendFactor::OneMinusDestinationColor: nativeFactors[factorIndex] = MTLBlendFactorOneMinusDestinationColor; break;
                case RHI::BlendFactor::DestinationAlpha: nativeFactors[factorIndex] = MTLBlendFactorDestinationAlpha; break;
                case RHI::BlendFactor::OneMinusDestinationAlpha: nativeFactors[factorIndex] = MTLBlendFactorOneMinusDestinationAlpha; break;
                default: return;
                }
            }
            target.sourceRGBBlendFactor = nativeFactors[0];
            target.destinationRGBBlendFactor = nativeFactors[1];
            target.sourceAlphaBlendFactor = nativeFactors[2];
            target.destinationAlphaBlendFactor = nativeFactors[3];
            const RHI::BlendOp operations[] = { source.colorOp, source.alphaOp };
            MTLBlendOperation nativeOperations[2] = {};
            for(uint32_t operationIndex = 0; operationIndex < 2; ++operationIndex)
            {
                switch(operations[operationIndex])
                {
                case RHI::BlendOp::Add: nativeOperations[operationIndex] = MTLBlendOperationAdd; break;
                case RHI::BlendOp::Subtract: nativeOperations[operationIndex] = MTLBlendOperationSubtract; break;
                case RHI::BlendOp::ReverseSubtract: nativeOperations[operationIndex] = MTLBlendOperationReverseSubtract; break;
                case RHI::BlendOp::Min: nativeOperations[operationIndex] = MTLBlendOperationMin; break;
                case RHI::BlendOp::Max: nativeOperations[operationIndex] = MTLBlendOperationMax; break;
                default: return;
                }
            }
            target.rgbBlendOperation = nativeOperations[0];
            target.alphaBlendOperation = nativeOperations[1];
            target.writeMask = MTLColorWriteMaskNone;
            if((source.writeMask & RHI::ColorWriteRed) != 0) target.writeMask |= MTLColorWriteMaskRed;
            if((source.writeMask & RHI::ColorWriteGreen) != 0) target.writeMask |= MTLColorWriteMaskGreen;
            if((source.writeMask & RHI::ColorWriteBlue) != 0) target.writeMask |= MTLColorWriteMaskBlue;
            if((source.writeMask & RHI::ColorWriteAlpha) != 0) target.writeMask |= MTLColorWriteMaskAlpha;
        }

        if(desc.depthStencilFormat != RHI::Format::Unknown)
        {
            pipeDesc.depthAttachmentPixelFormat = ToMTLFormat(desc.depthStencilFormat);
            if(desc.depthStencilFormat == RHI::Format::D24_UNORM_S8_UINT)
                pipeDesc.stencilAttachmentPixelFormat = ToMTLFormat(desc.depthStencilFormat);
        }

        m_impl->pipelineState = [mtlDevice newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
        if(!m_impl->pipelineState) { NSLog(@"파이프라인 생성 실패: %@", error); return; }

        MTLDepthStencilDescriptor* depthDesc = [MTLDepthStencilDescriptor new];
        depthDesc.depthWriteEnabled = desc.depthStencil.depthWriteEnable ? YES : NO;
        if(!desc.depthStencil.depthTestEnable)
        {
            depthDesc.depthCompareFunction = MTLCompareFunctionAlways;
        }
        else
        {
            switch(desc.depthStencil.depthCompareOp)
            {
            case RHI::CompareOp::Never: depthDesc.depthCompareFunction = MTLCompareFunctionNever; break;
            case RHI::CompareOp::Less: depthDesc.depthCompareFunction = MTLCompareFunctionLess; break;
            case RHI::CompareOp::Equal: depthDesc.depthCompareFunction = MTLCompareFunctionEqual; break;
            case RHI::CompareOp::LessEqual: depthDesc.depthCompareFunction = MTLCompareFunctionLessEqual; break;
            case RHI::CompareOp::Greater: depthDesc.depthCompareFunction = MTLCompareFunctionGreater; break;
            case RHI::CompareOp::NotEqual: depthDesc.depthCompareFunction = MTLCompareFunctionNotEqual; break;
            case RHI::CompareOp::GreaterEqual: depthDesc.depthCompareFunction = MTLCompareFunctionGreaterEqual; break;
            case RHI::CompareOp::Always: depthDesc.depthCompareFunction = MTLCompareFunctionAlways; break;
            default: return;
            }
        }

        const RHI::StencilFaceDesc* stencilFaces[] = { &desc.depthStencil.frontFace, &desc.depthStencil.backFace };
        MTLStencilDescriptor* nativeStencilFaces[] = { [MTLStencilDescriptor new], [MTLStencilDescriptor new] };
        for(uint32_t faceIndex = 0; faceIndex < 2; ++faceIndex)
        {
            const RHI::StencilFaceDesc& source = *stencilFaces[faceIndex];
            MTLStencilDescriptor* target = nativeStencilFaces[faceIndex];
            switch(source.compareOp)
            {
            case RHI::CompareOp::Never: target.stencilCompareFunction = MTLCompareFunctionNever; break;
            case RHI::CompareOp::Less: target.stencilCompareFunction = MTLCompareFunctionLess; break;
            case RHI::CompareOp::Equal: target.stencilCompareFunction = MTLCompareFunctionEqual; break;
            case RHI::CompareOp::LessEqual: target.stencilCompareFunction = MTLCompareFunctionLessEqual; break;
            case RHI::CompareOp::Greater: target.stencilCompareFunction = MTLCompareFunctionGreater; break;
            case RHI::CompareOp::NotEqual: target.stencilCompareFunction = MTLCompareFunctionNotEqual; break;
            case RHI::CompareOp::GreaterEqual: target.stencilCompareFunction = MTLCompareFunctionGreaterEqual; break;
            case RHI::CompareOp::Always: target.stencilCompareFunction = MTLCompareFunctionAlways; break;
            default: return;
            }
            const RHI::StencilOp operations[] = { source.failOp, source.depthFailOp, source.passOp };
            MTLStencilOperation nativeOperations[3] = {};
            for(uint32_t operationIndex = 0; operationIndex < 3; ++operationIndex)
            {
                switch(operations[operationIndex])
                {
                case RHI::StencilOp::Keep: nativeOperations[operationIndex] = MTLStencilOperationKeep; break;
                case RHI::StencilOp::Zero: nativeOperations[operationIndex] = MTLStencilOperationZero; break;
                case RHI::StencilOp::Replace: nativeOperations[operationIndex] = MTLStencilOperationReplace; break;
                case RHI::StencilOp::IncrementClamp: nativeOperations[operationIndex] = MTLStencilOperationIncrementClamp; break;
                case RHI::StencilOp::DecrementClamp: nativeOperations[operationIndex] = MTLStencilOperationDecrementClamp; break;
                case RHI::StencilOp::Invert: nativeOperations[operationIndex] = MTLStencilOperationInvert; break;
                case RHI::StencilOp::IncrementWrap: nativeOperations[operationIndex] = MTLStencilOperationIncrementWrap; break;
                case RHI::StencilOp::DecrementWrap: nativeOperations[operationIndex] = MTLStencilOperationDecrementWrap; break;
                default: return;
                }
            }
            target.stencilFailureOperation = nativeOperations[0];
            target.depthFailureOperation = nativeOperations[1];
            target.depthStencilPassOperation = nativeOperations[2];
            target.readMask = desc.depthStencil.stencilReadMask;
            target.writeMask = desc.depthStencil.stencilWriteMask;
        }
        if(desc.depthStencil.stencilTestEnable)
        {
            depthDesc.frontFaceStencil = nativeStencilFaces[0];
            depthDesc.backFaceStencil = nativeStencilFaces[1];
        }
        m_impl->depthStencilState = [mtlDevice newDepthStencilStateWithDescriptor:depthDesc];
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

    const RHI::RasterizationDesc& MetalPipeline::GetRasterization() const
    {
        return m_impl->rasterization;
    }

    uint32_t MetalPipeline::GetStencilReference() const
    {
        return m_impl->stencilReference;
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
        return m_impl->pipelineState != nil && m_impl->depthStencilState != nil;
    }
}
