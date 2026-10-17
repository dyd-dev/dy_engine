add_library(dy_extend_advanced_rendering STATIC
    AdvancedRendering.cpp
    AdvancedRenderingPasses.cpp
    EnvironmentLighting.cpp
    EnvironmentLightingModule.cpp
    ClusteredLighting.cpp
    ClusteredLightingModule.cpp
    SurfaceLightingModule.cpp
    FogModule.cpp
    TransparencyModule.cpp
    PostProcessModule.cpp
    ScreenSpaceLighting.cpp
    TemporalPostProcess.cpp
    ExposureMeter.cpp
    TransparentRendering.cpp
    VolumetricFog.cpp
)
add_library(dyf::AdvancedRendering ALIAS dy_extend_advanced_rendering)
target_link_libraries(dy_extend_advanced_rendering PUBLIC dy_engine)
# 코어의 비공개 정의에 의존하지 않고 선택한 셰이더 표현을 확장에도 지정한다.
foreach(backend IN ITEMS D3D12 METAL VULKAN)
    if(USE_${backend})
        target_compile_definitions(dy_extend_advanced_rendering PRIVATE ENABLE_${backend})
    endif()
endforeach()
# 내부 렌더 자원과 공용 계산을 재사용하되 이 경로를 공개 의존성으로 전파하지 않는다.
target_include_directories(dy_extend_advanced_rendering PRIVATE
    "${PROJECT_SOURCE_DIR}/src/dyf"
    "${PROJECT_SOURCE_DIR}/src"
)
include("${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ShaderAssets.cmake")
