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

        NSString* vertString = [NSString stringWithUTF8String:vertSrc];
        NSString* fragString = [NSString stringWithUTF8String:fragSrc];

        id<MTLLibrary> vertLib = [mtlDevice newLibraryWithSource:vertString
                                                         options:nil
                                                           error:&error];
        if(!vertLib) { NSLog(@"Vertex shader 컴파일 실패: %@", error); return; }

        id<MTLLibrary> fragLib = [mtlDevice newLibraryWithSource:fragString
                                                         options:nil
                                                           error:&error];
        if(!fragLib) { NSLog(@"Fragment shader 컴파일 실패: %@", error); return; }

        // Metal 셰이더 진입점은 main0
        id<MTLFunction> vertFunc = [vertLib newFunctionWithName:@"main0"];
        id<MTLFunction> fragFunc = [fragLib newFunctionWithName:@"main0"];

        if(!vertFunc) { NSLog(@"vertexShader 함수 못 찾음"); return; }
        if(!fragFunc) { NSLog(@"fragmentShader 함수 못 찾음"); return; }

        // 파이프라인 디스크립터 설정
        MTLRenderPipelineDescriptor* pipeDesc = [MTLRenderPipelineDescriptor new];
        pipeDesc.vertexFunction   = vertFunc;
        pipeDesc.fragmentFunction = fragFunc;

        // renderTargetFormat이 Unknown이면 기본값 BGRA8Unorm 사용
        if(desc.renderTargetFormat == RHI::Format::Unknown)
            pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        else
            pipeDesc.colorAttachments[0].pixelFormat = ToMTLFormat(desc.renderTargetFormat);

        if(desc.depthEnable && desc.depthStencilFormat != RHI::Format::Unknown)
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
}
