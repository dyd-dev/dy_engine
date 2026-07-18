#include "MetalPipeline.h"
#import <Metal/Metal.h>

namespace dy::Backends
{
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
            case RHI::Format::R8G8B8A8_UNORM:     return MTLPixelFormatRGBA8Unorm;
            case RHI::Format::D32_FLOAT:           return MTLPixelFormatDepth32Float;
            case RHI::Format::D24_UNORM_S8_UINT:   return MTLPixelFormatDepth24Unorm_Stencil8;
            default:                               return MTLPixelFormatInvalid;
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

        // MSL 소스 텍스트로 셰이더 로드
        const char* vertSrc = static_cast<const char*>(desc.vertexShader);
        const char* fragSrc = static_cast<const char*>(desc.pixelShader);

        if(vertSrc == nullptr || desc.vertexShaderSize == 0)
            return;

        NSString* vertString = [NSString stringWithUTF8String:vertSrc];

        id<MTLLibrary> vertLib = [mtlDevice newLibraryWithSource:vertString
                                                         options:nil
                                                           error:&error];
        if(!vertLib) { NSLog(@"Vertex shader 컴파일 실패: %@", error); return; }

        id<MTLFunction> vertFunc = [vertLib newFunctionWithName:@"main0"];
        if(!vertFunc) { NSLog(@"vertexShader 함수 못 찾음"); return; }

        id<MTLFunction> fragFunc = nil;
        if(fragSrc != nullptr && desc.pixelShaderSize > 0)
        {
            NSString* fragString = [NSString stringWithUTF8String:fragSrc];
            id<MTLLibrary> fragLib = [mtlDevice newLibraryWithSource:fragString options:nil error:&error];
            if(!fragLib) { NSLog(@"Fragment shader 컴파일 실패: %@", error); return; }
            fragFunc = [fragLib newFunctionWithName:@"main0"];
            if(!fragFunc) { NSLog(@"fragmentShader 함수 못 찾음"); return; }
        }

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
