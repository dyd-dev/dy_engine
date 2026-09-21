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
)
add_library(dyf::Model ALIAS dy_extend_model)
target_link_libraries(dy_extend_model PUBLIC dy_engine)
target_include_directories(dy_extend_model PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Model을 선택한 경우에만 변형 셰이더를 생성한다.
if(USE_D3D12 OR USE_VULKAN OR USE_METAL)
    set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
    set(model_library "")
    if(USE_METAL)
        set(model_library ModelMetalLibrary)
    endif()
    foreach(shadows IN ITEMS 1 0)
        set(suffix "")
        if(NOT shadows)
            set(suffix NoShadows)
        endif()
        dy_compile_shader(dy_extend_model ModelVertexShader${suffix} vert
            GLSL "${shader_dir}/mesh_vs.glsl" HLSL "${shader_dir}/mesh_vs.hlsl" METAL "${shader_dir}/mesh_vs.metal"
            ANONYMOUS SYMBOL kModelVertexShader${suffix} LIBRARY "${model_library}" INCLUDE_DIRS "${shader_dir}"
            DEFINES RENDERER_ENABLE_SHADOWS=${shadows} RENDERER_VERTEX_ENTRY=modelVertexShader${suffix})
    endforeach()
    dy_compile_shader(dy_extend_model ModelShadowShader vert
        GLSL "${shader_dir}/mesh_shadow_vs.glsl" HLSL "${shader_dir}/mesh_shadow_vs.hlsl" METAL "${shader_dir}/mesh_shadow_vs.metal"
        ANONYMOUS SYMBOL kModelShadowShader LIBRARY "${model_library}" INCLUDE_DIRS "${shader_dir}")
    if(USE_METAL)
        dy_link_shader_library(dy_extend_model ModelMetalLibrary)
        target_compile_definitions(dy_extend_model PRIVATE DY_MODEL_SHADERS_METAL=1)
    else()
        target_compile_definitions(dy_extend_model PRIVATE DY_MODEL_SHADERS_NATIVE=1)
        if(USE_VULKAN)
            dy_compile_shader(dy_extend_model ModelComputeShader comp
                GLSL "${shader_dir}/mesh_skinning_cs.glsl" ANONYMOUS SYMBOL kModelComputeShader INCLUDE_DIRS "${shader_dir}")
            target_compile_definitions(dy_extend_model PRIVATE DY_MODEL_SHADERS_COMPUTE=1)
        endif()
    endif()
endif()

# fastgltf 자동 다운로드 설정
FetchContent_Declare(
    fastgltf
    GIT_REPOSITORY "https://github.com/spnda/fastgltf.git"
    GIT_TAG "v0.9.0" # 최신 버전
)
FetchContent_MakeAvailable(fastgltf)
target_link_libraries(dy_extend_model PRIVATE fastgltf::fastgltf)

# ufbx 자동 다운로드 설정
FetchContent_Declare(
    ufbx
    GIT_REPOSITORY "https://github.com/ufbx/ufbx.git"
    GIT_TAG "fcc5d6ba444cfd3eb80677dba5e37e493941abe5"
)
FetchContent_MakeAvailable(ufbx)
target_include_directories(dy_extend_model PRIVATE ${ufbx_SOURCE_DIR})
set_source_files_properties("${ufbx_SOURCE_DIR}/ufbx.c" PROPERTIES LANGUAGE CXX)
target_sources(dy_extend_model PRIVATE "${ufbx_SOURCE_DIR}/ufbx.c")
