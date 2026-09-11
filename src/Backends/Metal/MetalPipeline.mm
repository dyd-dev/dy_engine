#include "MetalPipeline.h"
#import <Metal/Metal.h>

namespace dy::Backends
{
    struct MetalPipeline::Impl
    {
        id<MTLRenderPipelineState> pipelineState    = nil;
        id<MTLDepthStencilState>   depthStencilState = nil;
    };

    static MTLPixelFormat ToMTLFormat(RHI::Format format)
    {
        switch(format)
        {
            case RHI::Format::R8G8B8A8_UNORM:     return MTLPixelFormatRGBA8Unorm;
            case RHI::Format::B8G8R8A8_UNORM:     return MTLPixelFormatBGRA8Unorm;
            case RHI::Format::R8G8B8A8_UNORM_SRGB:return MTLPixelFormatRGBA8Unorm_sRGB;
            case RHI::Format::B8G8R8A8_UNORM_SRGB:return MTLPixelFormatBGRA8Unorm_sRGB;
            case RHI::Format::R16G16B16A16_FLOAT: return MTLPixelFormatRGBA16Float;
            case RHI::Format::R32G32B32A32_FLOAT: return MTLPixelFormatRGBA32Float;
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

        // MSL 소스 텍스트로 셰이더 로드
        const char* vertSrc = static_cast<const char*>(desc.vertexShader);
        const char* fragSrc = static_cast<const char*>(desc.pixelShader);

        if(vertSrc == nullptr || desc.vertexShaderSize == 0)
        {
            NSLog(@"Vertex shader source is empty");
            return;
        }
        NSString* vertString = [[NSString alloc]
            initWithBytes:vertSrc
            length:desc.vertexShaderSize
            encoding:NSUTF8StringEncoding];
        NSString* fragString = nil;
        if(fragSrc != nullptr && desc.pixelShaderSize > 0)
        {
            fragString = [[NSString alloc]
                initWithBytes:fragSrc
                length:desc.pixelShaderSize
                encoding:NSUTF8StringEncoding];
        }
        if(vertString == nil || (fragSrc != nullptr && fragString == nil))
        {
            NSLog(@"Metal shader source is not valid UTF-8");
            return;
        }

        id<MTLLibrary> vertLib = [mtlDevice newLibraryWithSource:vertString
                                                         options:nil
                                                           error:&error];
        if(!vertLib) { NSLog(@"Vertex shader 컴파일 실패: %@", error); return; }

        id<MTLLibrary> fragLib = nil;
        if(fragString != nil)
        {
            error = nil;
            fragLib = [mtlDevice newLibraryWithSource:fragString options:nil error:&error];
            if(!fragLib) { NSLog(@"Fragment shader 컴파일 실패: %@", error); return; }
        }

        // Metal 셰이더 진입점은 main0
        id<MTLFunction> vertFunc = [vertLib newFunctionWithName:@"main0"];
        id<MTLFunction> fragFunc = fragLib != nil ? [fragLib newFunctionWithName:@"main0"] : nil;

        if(!vertFunc) { NSLog(@"vertexShader 함수 못 찾음"); return; }
        if(fragLib != nil && !fragFunc) { NSLog(@"fragmentShader 함수 못 찾음"); return; }

        // 파이프라인 디스크립터 설정
        MTLRenderPipelineDescriptor* pipeDesc = [MTLRenderPipelineDescriptor new];
        pipeDesc.vertexFunction   = vertFunc;
        pipeDesc.fragmentFunction = fragFunc;

        if(desc.renderTargetFormat != RHI::Format::Unknown)
            pipeDesc.colorAttachments[0].pixelFormat = ToMTLFormat(desc.renderTargetFormat);

        if(desc.depthStencilFormat != RHI::Format::Unknown)
            pipeDesc.depthAttachmentPixelFormat = ToMTLFormat(desc.depthStencilFormat);

        m_impl->pipelineState = [mtlDevice newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
        if(!m_impl->pipelineState) { NSLog(@"파이프라인 생성 실패: %@", error); return; }

        // Always bind an explicit state. Leaving this nil for an overlay pipeline
        // would keep the scene's previous depth state active on the encoder.
        MTLDepthStencilDescriptor* depthDesc = [MTLDepthStencilDescriptor new];
        depthDesc.depthCompareFunction = desc.depthEnable
            ? MTLCompareFunctionLess
            : MTLCompareFunctionAlways;
        depthDesc.depthWriteEnabled = desc.depthEnable ? YES : NO;
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

    bool MetalPipeline::IsValid() const
    {
        return m_impl != nullptr && m_impl->pipelineState != nil;
    }
}
