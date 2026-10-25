#include "RendererInternal.h"
#if defined(ENABLE_METAL) // Metal 빌드의 라이브러리 바이트 헤더를 포함한다.
#include "StockMetalLibrary.h"
#include "BindlessMetalLibrary.h"
#include "CanvasLibrary.h"
#include "ToneMapLibrary.h"
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN) // D3D12/Vulkan 빌드의 컴파일된 변형 헤더를 포함한다.
#include "StockVertexShader.h"
#include "StockInstancedVertexShader.h"
#include "StockInstancedVertexShaderNoShadows.h"
#include "StockVertexShaderNoShadows.h"
#include "StockFragmentShader.h"
#include "StockFragmentShaderNoShadows.h"
#include "StockShadowVertexShader.h"
#include "StockInstancedShadowVertexShader.h"
#include "StockShadowFragmentShader.h"
#include "BindlessFragment.h"
#include "BindlessFragmentNoShadows.h"
#include "CanvasVertex.h"
#include "CanvasFragment.h"
#include "ToneMapVertex.h"
#include "ToneMapFragment.h"
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
#include "StockDepthPrepassFragment.h"
#include "StockDepthPrepassFragmentNoShadows.h"
#include "BindlessDepthPrepassFragment.h"
#include "BindlessDepthPrepassFragmentNoShadows.h"
#include "StockNoAreaFragment.h"
#include "StockNoAreaFragmentNoShadows.h"
#include "BindlessNoAreaFragment.h"
#include "BindlessNoAreaFragmentNoShadows.h"
#include "StockNoAreaDepthPrepassFragment.h"
#include "StockNoAreaDepthPrepassFragmentNoShadows.h"
#include "BindlessNoAreaDepthPrepassFragment.h"
#include "BindlessNoAreaDepthPrepassFragmentNoShadows.h"
#endif

namespace dyf
{
// 알파 마스크 등을 처리하는 내장 그림자 픽셀 셰이더 설명을 반환한다.
RHI::ShaderDesc Renderer::Impl::DefaultShadowFragment()
{
#if defined(ENABLE_METAL) // Metal에서는 공유 라이브러리의 그림자 픽셀 진입점을 선택한다.
    return {RHI::ShaderStage::Fragment,"shadowFragmentShader",kStockMetalLibrary,kStockMetalLibrarySize};
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN) // D3D12/Vulkan에서는 그림자 픽셀 바이트와 main 진입점을 선택한다.
    return {RHI::ShaderStage::Fragment,"main",kStockShadowFragmentShader,kStockShadowFragmentShaderSize};
#else
    RHI::ShaderDesc desc;desc.stage=RHI::ShaderStage::Fragment;return desc;
#endif
}

RHI::ShaderDesc Renderer::Impl::DefaultDepthPrepassFragment(bool shadows,bool bindless)
{
#if defined(ENABLE_METAL)
    constexpr const char* entry="depthPrepassFragment";
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
    constexpr const char* entry="main";
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
    if(bindless)
        return shadows
            ? RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kBindlessDepthPrepassFragment,kBindlessDepthPrepassFragmentSize}
            : RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kBindlessDepthPrepassFragmentNoShadows,kBindlessDepthPrepassFragmentNoShadowsSize};
    return shadows
        ? RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kStockDepthPrepassFragment,kStockDepthPrepassFragmentSize}
        : RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kStockDepthPrepassFragmentNoShadows,kStockDepthPrepassFragmentNoShadowsSize};
#else
    (void)shadows;(void)bindless;
    RHI::ShaderDesc desc;desc.stage=RHI::ShaderStage::Fragment;return desc;
#endif
}
RHI::ShaderDesc Renderer::Impl::DefaultNoAreaFragment(bool shadows,bool bindless,bool depthPrepass)
{
#if defined(ENABLE_METAL)
    constexpr const char* entry="noAreaFragment";
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
    constexpr const char* entry="main";
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN)
    if(depthPrepass)
    {
        if(bindless)
            return shadows
                ? RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kBindlessNoAreaDepthPrepassFragment,kBindlessNoAreaDepthPrepassFragmentSize}
                : RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kBindlessNoAreaDepthPrepassFragmentNoShadows,kBindlessNoAreaDepthPrepassFragmentNoShadowsSize};
        return shadows
            ? RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kStockNoAreaDepthPrepassFragment,kStockNoAreaDepthPrepassFragmentSize}
            : RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kStockNoAreaDepthPrepassFragmentNoShadows,kStockNoAreaDepthPrepassFragmentNoShadowsSize};
    }
    if(bindless)
        return shadows
            ? RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kBindlessNoAreaFragment,kBindlessNoAreaFragmentSize}
            : RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kBindlessNoAreaFragmentNoShadows,kBindlessNoAreaFragmentNoShadowsSize};
    return shadows
        ? RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kStockNoAreaFragment,kStockNoAreaFragmentSize}
        : RHI::ShaderDesc{RHI::ShaderStage::Fragment,entry,kStockNoAreaFragmentNoShadows,kStockNoAreaFragmentNoShadowsSize};
#else
    (void)shadows;(void)bindless;(void)depthPrepass;
    RHI::ShaderDesc desc;desc.stage=RHI::ShaderStage::Fragment;return desc;
#endif
}

// 메시·그림자·Canvas·톤 매핑의 기본 ShaderDesc를 묶어 반환하며, Metal은 라이브러리 진입점, D3D12/Vulkan은 변형별 바이트를 선택한다.
RendererShaderDesc Renderer::Impl::DefaultShaders(bool shadows, bool bindless, bool instanced)
{
    RendererShaderDesc shaders;
#if defined(ENABLE_METAL) // Metal 빌드의 라이브러리와 이름으로 각 셰이더 역할을 구성한다.
    shaders.meshVertex = {RHI::ShaderStage::Vertex, shadows ? "vertexShader" : "vertexShaderNoShadows", kStockMetalLibrary, kStockMetalLibrarySize};
    if(instanced) shaders.meshVertex.entryPoint=shadows ? "instancedVertexShader" : "instancedVertexShaderNoShadows"; // Metal 인스턴싱이면 모델 자료를 인스턴스 경로에서 읽는 진입점으로 바꾼다.
    // Metal에서는 bindless 선택에 따라 다른 라이브러리와 픽셀 진입점을 사용한다.
    shaders.meshFragment = bindless
        ? RHI::ShaderDesc{RHI::ShaderStage::Fragment, shadows ? "bindlessFragment1" : "bindlessFragment0", kBindlessMetalLibrary, kBindlessMetalLibrarySize}
        : RHI::ShaderDesc{RHI::ShaderStage::Fragment, shadows ? "fragmentShader" : "fragmentShaderNoShadows", kStockMetalLibrary, kStockMetalLibrarySize};
    shaders.shadowVertex = {RHI::ShaderStage::Vertex, instanced ? "instancedShadowVertexShader" : "shadowVertexShader", kStockMetalLibrary, kStockMetalLibrarySize};
    shaders.canvasVertex = {RHI::ShaderStage::Vertex, "canvasVertex", kCanvasLibrary, kCanvasLibrarySize};
    shaders.canvasFragment = {RHI::ShaderStage::Fragment, "canvasFragment", kCanvasLibrary, kCanvasLibrarySize};
    shaders.toneMapVertex = {RHI::ShaderStage::Vertex, "toneVertex", kToneMapLibrary, kToneMapLibrarySize};
    shaders.toneMapFragment = {RHI::ShaderStage::Fragment, "toneFragment", kToneMapLibrary, kToneMapLibrarySize};
#elif defined(ENABLE_D3D12) || defined(ENABLE_VULKAN) // D3D12/Vulkan 빌드의 역할별 바이트 배열을 구성한다.
    shaders.meshVertex = shadows
        ? RHI::ShaderDesc{RHI::ShaderStage::Vertex, "main", kStockVertexShader, kStockVertexShaderSize}
        : RHI::ShaderDesc{RHI::ShaderStage::Vertex, "main", kStockVertexShaderNoShadows, kStockVertexShaderNoShadowsSize};
    if(instanced) shaders.meshVertex=shadows // 인스턴싱이면 그림자 사용 여부에 맞는 인스턴싱 정점 바이트를 선택한다.
        ? RHI::ShaderDesc{RHI::ShaderStage::Vertex,"main",kStockInstancedVertexShader,kStockInstancedVertexShaderSize}
        : RHI::ShaderDesc{RHI::ShaderStage::Vertex,"main",kStockInstancedVertexShaderNoShadows,kStockInstancedVertexShaderNoShadowsSize};
    if(bindless) // bindless이면 텍스처 테이블을 읽는 픽셀 변형을 선택한다.
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
    // 백엔드 셰이더 바이트가 없는 빌드에서도 정점·픽셀 단계 정보는 남겨 사용자 재정의 입력의 역할을 구분한다.
    shaders.meshVertex.stage=shaders.shadowVertex.stage=shaders.canvasVertex.stage=shaders.toneMapVertex.stage=RHI::ShaderStage::Vertex;
    shaders.meshFragment.stage=shaders.canvasFragment.stage=shaders.toneMapFragment.stage=RHI::ShaderStage::Fragment;
    (void)shadows;
    (void)bindless;
    (void)instanced;
#endif
    return shaders;
}
}
