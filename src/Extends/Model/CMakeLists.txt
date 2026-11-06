# Dependencies.
# Keep the simdjson workaround local to fastgltf's dependency configuration.
function(dy_fetch_fastgltf)
    FetchContent_Declare(
        fastgltf
        GIT_REPOSITORY "https://github.com/spnda/fastgltf.git"
        GIT_TAG "v0.9.0"
    )
    # fastgltf v0.9.0 is validated with its pinned simdjson 3.12.3. Do not let a
    # host package manager silently substitute a newer, incompatible header.
    set(CMAKE_DISABLE_FIND_PACKAGE_simdjson ON)
    FetchContent_MakeAvailable(fastgltf)
endfunction()
if(NOT TARGET fastgltf::fastgltf)
    dy_fetch_fastgltf()
endif()

# ufbx 자동 다운로드 설정
FetchContent_Declare(
    ufbx
    GIT_REPOSITORY "https://github.com/ufbx/ufbx.git"
    GIT_TAG "fcc5d6ba444cfd3eb80677dba5e37e493941abe5"
)
FetchContent_MakeAvailable(ufbx)

# Target and sources.
add_library(dy_extend_model STATIC
    Animation.cpp
    ModelShaderAssets.cpp
    ModelRenderer.cpp
    Model.cpp
    ModelScene.cpp
    ModelLoader.cpp
    ModelLoaderCommon.cpp
    GltfModelLoader.cpp
    UfbxModelLoader.cpp
    "${ufbx_SOURCE_DIR}/ufbx.c"
)
add_library(dyf::Model ALIAS dy_extend_model)

# Library dependencies.
target_link_libraries(dy_extend_model PUBLIC dy_engine PRIVATE fastgltf::fastgltf)

# Include paths and compiler settings.
target_include_directories(dy_extend_model PRIVATE "${ufbx_SOURCE_DIR}")
set_source_files_properties("${ufbx_SOURCE_DIR}/ufbx.c" PROPERTIES LANGUAGE CXX)

# Generated model shaders.
include("${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ShaderAssets.cmake")

if(DY_BUILD_CONFORMANCE_TESTS)
    add_executable(ModelRuntimeReuseTests tests/ModelRuntimeReuseTests.cpp "${PROJECT_SOURCE_DIR}/src/Backends/Null/NullDevice.cpp")
    target_link_libraries(ModelRuntimeReuseTests PRIVATE dyf::Model)
    target_include_directories(ModelRuntimeReuseTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    set_target_properties(ModelRuntimeReuseTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
    add_test(NAME ModelRuntimeReuse_animation COMMAND ModelRuntimeReuseTests animation)
    add_test(NAME ModelRuntimeReuse_deformation COMMAND ModelRuntimeReuseTests deformation)
    add_test(NAME ModelRuntimeReuse_aliases COMMAND ModelRuntimeReuseTests aliases)
    add_test(NAME ModelRuntimeReuse_controls COMMAND ModelRuntimeReuseTests controls)
    add_test(NAME ModelRuntimeReuse_mixed COMMAND ModelRuntimeReuseTests mixed)
    add_test(NAME ModelRuntimeReuse_creation_recovery COMMAND ModelRuntimeReuseTests creation-recovery)
    add_test(NAME ModelRuntimeReuse_binding_recovery COMMAND ModelRuntimeReuseTests binding-recovery)
    add_executable(ModelConformanceTests tests/ModelConformanceTests.cpp)
    target_link_libraries(ModelConformanceTests PRIVATE dyf::Model)
    target_include_directories(ModelConformanceTests PRIVATE "${ufbx_SOURCE_DIR}")
    set_target_properties(ModelConformanceTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
    add_test(NAME ModelConformance COMMAND ModelConformanceTests)
    if(TARGET dyf::AdvancedRendering)
        add_executable(RenderingConformanceTests tests/RenderingConformanceTests.cpp)
        target_link_libraries(RenderingConformanceTests PRIVATE dyf::Model dyf::AdvancedRendering)
        target_include_directories(RenderingConformanceTests PRIVATE
            "${stb_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
        if(NOT DY_BACKEND_NORMALIZED STREQUAL "NULL")
            set(DY_STOCK_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/Shaders")
            set(DY_STOCK_SHADER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/conformance")
            if(CMAKE_CONFIGURATION_TYPES)
                string(APPEND DY_STOCK_SHADER_GENERATED_DIR "/$<CONFIG>")
            endif()
            set(DY_STOCK_SHADER_HEADERS)
            if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
                dy_embed_stock_metal_library(SurfaceOwnerProbe kSurfaceOwnerProbe surface_owner_probe)
                target_compile_definitions(RenderingConformanceTests PRIVATE DY_SURFACE_OWNER_PROBE_METAL=1)
            else()
                dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/surface_owner_probe.${model_shader_extension}"
                    vert SurfaceOwnerProbeVertex kSurfaceOwnerProbeVertex -DPROBE_VERTEX=1)
                dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/surface_owner_probe.${model_shader_extension}"
                    frag SurfaceOwnerProbeFragment kSurfaceOwnerProbeFragment)
            endif()
            target_sources(RenderingConformanceTests PRIVATE ${DY_STOCK_SHADER_HEADERS})
            target_include_directories(RenderingConformanceTests PRIVATE "${DY_STOCK_SHADER_GENERATED_DIR}")
            target_compile_definitions(RenderingConformanceTests PRIVATE DY_SURFACE_OWNER_PROBE=1)
        endif()
        set_target_properties(RenderingConformanceTests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
        add_test(NAME RenderingCPUConformance COMMAND RenderingConformanceTests --cpu-only)
    endif()
endif()
