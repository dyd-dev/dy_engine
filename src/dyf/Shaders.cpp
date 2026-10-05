#include "dyf/Renderer.h"
#if defined(ENABLE_METAL)
#include "StockMetalLibrary.h"
#include "BindlessMetalLibrary.h"
#include "CanvasLibrary.h"
#include "ToneMapLibrary.h"
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
#include "StockVertexShader.h"
#include "StockInstancedVertexShader.h"
#include "StockInstancedVertexShaderNoShadows.h"
#include "StockVertexShaderNoShadows.h"
#include "StockFragmentShader.h"
#include "StockFragmentShaderNoShadows.h"
#include "StockShadowVertexShader.h"
#include "StockInstancedShadowVertexShader.h"
#include "BindlessFragment.h"
#include "BindlessFragmentNoShadows.h"
#include "CanvasVertex.h"
#include "CanvasFragment.h"
#include "ToneMapVertex.h"
#include "ToneMapFragment.h"
#endif

namespace dyf
{
// 선택한 빌드의 셰이더 바이트를 기존 RHI 입력에 직접 담는다.
RendererShaderDesc Renderer::DefaultShaders(bool shadows, bool bindless, bool instanced)
{
    RendererShaderDesc shaders;
#if defined(ENABLE_METAL)
    shaders.meshVertex = {RHI::ShaderStage::Vertex, shadows ? "vertexShader" : "vertexShaderNoShadows", kStockMetalLibrary, kStockMetalLibrarySize};
    if(instanced) shaders.meshVertex.entryPoint=shadows ? "instancedVertexShader" : "instancedVertexShaderNoShadows";
    shaders.meshFragment = bindless
        ? RHI::ShaderDesc{RHI::ShaderStage::Fragment, shadows ? "bindlessFragment1" : "bindlessFragment0", kBindlessMetalLibrary, kBindlessMetalLibrarySize}
        : RHI::ShaderDesc{RHI::ShaderStage::Fragment, shadows ? "fragmentShader" : "fragmentShaderNoShadows", kStockMetalLibrary, kStockMetalLibrarySize};
    shaders.shadowVertex = {RHI::ShaderStage::Vertex, instanced ? "instancedShadowVertexShader" : "shadowVertexShader", kStockMetalLibrary, kStockMetalLibrarySize};
    shaders.canvasVertex = {RHI::ShaderStage::Vertex, "canvasVertex", kCanvasLibrary, kCanvasLibrarySize};
    shaders.canvasFragment = {RHI::ShaderStage::Fragment, "canvasFragment", kCanvasLibrary, kCanvasLibrarySize};
    shaders.toneMapVertex = {RHI::ShaderStage::Vertex, "toneVertex", kToneMapLibrary, kToneMapLibrarySize};
    shaders.toneMapFragment = {RHI::ShaderStage::Fragment, "toneFragment", kToneMapLibrary, kToneMapLibrarySize};
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
    shaders.meshVertex = shadows
        ? RHI::ShaderDesc{RHI::ShaderStage::Vertex, "main", kStockVertexShader, kStockVertexShaderSize}
        : RHI::ShaderDesc{RHI::ShaderStage::Vertex, "main", kStockVertexShaderNoShadows, kStockVertexShaderNoShadowsSize};
    if(instanced) shaders.meshVertex=shadows
        ? RHI::ShaderDesc{RHI::ShaderStage::Vertex,"main",kStockInstancedVertexShader,kStockInstancedVertexShaderSize}
        : RHI::ShaderDesc{RHI::ShaderStage::Vertex,"main",kStockInstancedVertexShaderNoShadows,kStockInstancedVertexShaderNoShadowsSize};
    if(bindless)
        shaders.meshFragment = shadows
            ? RHI::ShaderDesc{RHI::ShaderStage::Fragment, "main", kBindlessFragment, kBindlessFragmentSize}
            : RHI::ShaderDesc{RHI::ShaderStage::Fragment, "main", kBindlessFragmentNoShadows, kBindlessFragmentNoShadowsSize};
    else
        shaders.meshFragment = shadows
            ? RHI::ShaderDesc{RHI::ShaderStage::Fragment, "main", kStockFragmentShader, kStockFragmentShaderSize}
            : RHI::ShaderDesc{RHI::ShaderStage::Fragment, "main", kStockFragmentShaderNoShadows, kStockFragmentShaderNoShadowsSize};
    shaders.shadowVertex = instanced
        ? RHI::ShaderDesc{RHI::ShaderStage::Vertex, "main", kStockInstancedShadowVertexShader, kStockInstancedShadowVertexShaderSize}
        : RHI::ShaderDesc{RHI::ShaderStage::Vertex, "main", kStockShadowVertexShader, kStockShadowVertexShaderSize};
    shaders.canvasVertex = {RHI::ShaderStage::Vertex, "main", kCanvasVertex, kCanvasVertexSize};
    shaders.canvasFragment = {RHI::ShaderStage::Fragment, "main", kCanvasFragment, kCanvasFragmentSize};
    shaders.toneMapVertex = {RHI::ShaderStage::Vertex, "main", kToneMapVertex, kToneMapVertexSize};
    shaders.toneMapFragment = {RHI::ShaderStage::Fragment, "main", kToneMapFragment, kToneMapFragmentSize};
#else
    // Null에서도 사용자 셰이더 재정의의 단계 정보는 유지한다.
    shaders.meshVertex.stage=shaders.shadowVertex.stage=shaders.canvasVertex.stage=shaders.toneMapVertex.stage=RHI::ShaderStage::Vertex;
    shaders.meshFragment.stage=shaders.canvasFragment.stage=shaders.toneMapFragment.stage=RHI::ShaderStage::Fragment;
    (void)shadows;
    (void)bindless;
    (void)instanced;
#endif
    return shaders;
}
}
