add_library(dy_extend_advanced_rendering STATIC
    AdvancedRendering.cpp
    AdvancedRenderingPasses.cpp
    AreaLightingLtc.cpp
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
# 내부 렌더 자원과 공용 계산을 재사용하되 이 경로를 공개 의존성으로 전파하지 않는다.
target_include_directories(dy_extend_advanced_rendering PRIVATE
    "${PROJECT_SOURCE_DIR}/src/dyf"
    "${PROJECT_SOURCE_DIR}/src"
)
# 코어의 비공개 정의에 의존하지 않고 선택한 셰이더 표현을 확장에도 지정한다.
if(NOT DY_BACKEND_NORMALIZED STREQUAL "NULL")
    target_compile_definitions(dy_extend_advanced_rendering PRIVATE "ENABLE_${DY_BACKEND_NORMALIZED}")
endif()

# LTC 데이터는 이 선택 확장에서만 내장한다.
set(DY_LTC_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/lighting")
foreach(table IN ITEMS Matrix Amplitude)
    if(table STREQUAL "Matrix")
        set(ltc_table "${CMAKE_CURRENT_SOURCE_DIR}/Assets/ltc/ltc_1.dds")
    else()
        set(ltc_table "${CMAKE_CURRENT_SOURCE_DIR}/Assets/ltc/ltc_2.dds")
    endif()
    set(header "${DY_LTC_GENERATED_DIR}/AreaLtc${table}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_LTC_GENERATED_DIR}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${ltc_table}" "-DOUTPUT=${header}" "-DSYMBOL=kAreaLtc${table}"
            -P "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        DEPENDS "${ltc_table}" "${DY_STOCK_SHADER_EMBED_SCRIPT}" VERBATIM)
    target_sources(dy_extend_advanced_rendering PRIVATE "${header}")
endforeach()
target_include_directories(dy_extend_advanced_rendering PRIVATE "${DY_LTC_GENERATED_DIR}")

include("${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ShaderAssets.cmake")
