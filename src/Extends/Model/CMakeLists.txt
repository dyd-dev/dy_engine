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
        set_target_properties(RenderingConformanceTests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")
        add_test(NAME RenderingCPUConformance COMMAND RenderingConformanceTests --cpu-only)
    endif()
endif()
