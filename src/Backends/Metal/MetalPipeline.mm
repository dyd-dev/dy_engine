//
//  MetalPipeline.mm
//  
//
//  Created by 정준혁 on 4/8/26.
//

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

        // 1. 셰이더 로드 (SPIR-V 바이트코드 → Metal은 .metallib or MSL 소스)
        dispatch_data_t vertData = dispatch_data_create(
            desc.vertexShader, desc.vertexShaderSize,
            nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        dispatch_data_t fragData = dispatch_data_create(
            desc.pixelShader, desc.pixelShaderSize,
            nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);

        NSError* error = nil;
        id<MTLLibrary> vertLib = [mtlDevice newLibraryWithData:vertData error:&error];
        id<MTLLibrary> fragLib = [mtlDevice newLibraryWithData:fragData error:&error];

        id<MTLFunction> vertFunc = [vertLib newFunctionWithName:@"vertexShader"];
        id<MTLFunction> fragFunc = [fragLib newFunctionWithName:@"fragmentShader"];

        // 2. 파이프라인 디스크립터 설정
        MTLRenderPipelineDescriptor* pipeDesc = [MTLRenderPipelineDescriptor new];
        pipeDesc.vertexFunction   = vertFunc;
        pipeDesc.fragmentFunction = fragFunc;
        pipeDesc.colorAttachments[0].pixelFormat = ToMTLFormat(desc.renderTargetFormat);

        if(desc.depthEnable)
            pipeDesc.depthAttachmentPixelFormat = ToMTLFormat(desc.depthStencilFormat);

        if(desc.wireframe)
            ; // Metal은 파이프라인 단계에서 wireframe 설정 없음 → CommandList에서 setTriangleFillMode로 처리

        m_impl->pipelineState = [mtlDevice newRenderPipelineStateWithDescriptor:pipeDesc error:&error];

        // 3. DepthStencil 상태 생성
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
