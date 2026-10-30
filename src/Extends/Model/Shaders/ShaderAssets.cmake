# 스키닝은 선택 Model 확장의 기능이며 기본 셰이더 타겟에는 포함하지 않는다.
if(DY_BACKEND_NORMALIZED STREQUAL "NULL")
    return()
endif()

dy_configure_shader_toolchain()
set(DY_MODEL_SHADER_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(DY_MODEL_SHADER_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated/model_shaders")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND DY_MODEL_SHADER_GENERATED_DIR "/$<CONFIG>")
endif()
set(DY_MODEL_SHADER_EMBED_SCRIPT "${PROJECT_SOURCE_DIR}/cmake/CompileShaders.cmake")
file(GLOB DY_MODEL_SHADER_HELPERS CONFIGURE_DEPENDS
    "${DY_MODEL_SHADER_SOURCE_DIR}/*.inc"
    "${DY_MODEL_SHADER_SOURCE_DIR}/Skinning.*")
set(DY_MODEL_SHADER_HEADERS)

function(dy_embed_model_shader source stage name)
    set(binary "${DY_MODEL_SHADER_GENERATED_DIR}/${name}.bin")
    set(shader_defines ${ARGN})
    if(DY_BACKEND_NORMALIZED STREQUAL "D3D12")
        if(stage STREQUAL "vert")
            set(profile vs_6_0)
        elseif(stage STREQUAL "comp")
            set(profile cs_6_0)
        else()
            message(FATAL_ERROR "Unsupported Model shader stage: ${stage}")
        endif()
        add_custom_command(
            OUTPUT "${binary}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_MODEL_SHADER_GENERATED_DIR}"
            COMMAND "${DY_DXC}" -T "${profile}" -E main ${shader_defines}
                -I "${DY_MODEL_SHADER_SOURCE_DIR}" -Fo "${binary}" "${source}"
            DEPENDS "${source}" ${DY_MODEL_SHADER_HELPERS}
            VERBATIM)
    elseif(DY_BACKEND_NORMALIZED STREQUAL "VULKAN")
        add_custom_command(
            OUTPUT "${binary}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_MODEL_SHADER_GENERATED_DIR}"
            COMMAND "${DY_GLSLC}" "$<IF:$<CONFIG:Debug>,-O0,-O>" "-fshader-stage=${stage}" ${shader_defines}
                -I "${DY_MODEL_SHADER_SOURCE_DIR}" "${source}" -o "${binary}"
            DEPENDS "${source}" ${DY_MODEL_SHADER_HELPERS}
            VERBATIM)
    else()
        message(FATAL_ERROR "No Model shader compiler configured for ${DY_BACKEND_NORMALIZED}")
    endif()
    set(header "${DY_MODEL_SHADER_GENERATED_DIR}/${name}.h")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${binary}" "-DOUTPUT=${header}" "-DSYMBOL=k${name}"
            -P "${DY_MODEL_SHADER_EMBED_SCRIPT}"
        DEPENDS "${binary}" "${DY_MODEL_SHADER_EMBED_SCRIPT}"
        VERBATIM)
    set(DY_MODEL_SHADER_HEADERS "${DY_MODEL_SHADER_HEADERS};${header}" PARENT_SCOPE)
endfunction()

if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    set(model_air_files)
    foreach(enable_shadows IN ITEMS 1 0)
        if(enable_shadows)
            set(vertex_entry modelVertexShader)
        else()
            set(vertex_entry modelVertexShaderNoShadows)
        endif()
        set(source "${DY_MODEL_SHADER_SOURCE_DIR}/mesh_vs.metal")
        set(air "${DY_MODEL_SHADER_GENERATED_DIR}/${vertex_entry}.air")
        add_custom_command(
            OUTPUT "${air}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_MODEL_SHADER_GENERATED_DIR}"
            COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS} -I "${DY_MODEL_SHADER_SOURCE_DIR}"
                "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}" "-DRENDERER_VERTEX_ENTRY=${vertex_entry}"
                -c "${source}" -o "${air}"
            DEPENDS "${source}" ${DY_MODEL_SHADER_HELPERS}
            VERBATIM)
        list(APPEND model_air_files "${air}")
    endforeach()
    set(source "${DY_MODEL_SHADER_SOURCE_DIR}/mesh_shadow_vs.metal")
    set(air "${DY_MODEL_SHADER_GENERATED_DIR}/modelShadowVertexShader.air")
    add_custom_command(
        OUTPUT "${air}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_MODEL_SHADER_GENERATED_DIR}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS} -I "${DY_MODEL_SHADER_SOURCE_DIR}"
            -c "${source}" -o "${air}"
        DEPENDS "${source}" ${DY_MODEL_SHADER_HELPERS}
        VERBATIM)
    list(APPEND model_air_files "${air}")
    set(source "${DY_MODEL_SHADER_SOURCE_DIR}/mesh_skinning_cs.metal")
    set(air "${DY_MODEL_SHADER_GENERATED_DIR}/modelSkinningCompute.air")
    add_custom_command(
        OUTPUT "${air}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_MODEL_SHADER_GENERATED_DIR}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS} -I "${DY_MODEL_SHADER_SOURCE_DIR}"
            -c "${source}" -o "${air}"
        DEPENDS "${source}" ${DY_MODEL_SHADER_HELPERS}
        VERBATIM)
    list(APPEND model_air_files "${air}")
    set(library "${DY_MODEL_SHADER_GENERATED_DIR}/Model.metallib")
    set(header "${DY_MODEL_SHADER_GENERATED_DIR}/ModelMetalLibrary.h")
    add_custom_command(
        OUTPUT "${header}"
        BYPRODUCTS "${library}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metallib ${model_air_files} -o "${library}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${library}" "-DOUTPUT=${header}" -DSYMBOL=kModelMetalLibrary
            -P "${DY_MODEL_SHADER_EMBED_SCRIPT}"
        DEPENDS ${model_air_files} "${DY_MODEL_SHADER_EMBED_SCRIPT}"
        VERBATIM)
    list(APPEND DY_MODEL_SHADER_HEADERS "${header}")
    target_compile_definitions(dy_extend_model PRIVATE DY_MODEL_SHADERS_METAL=1)
else()
    if(DY_BACKEND_NORMALIZED STREQUAL "D3D12")
        set(model_shader_extension hlsl)
    elseif(DY_BACKEND_NORMALIZED STREQUAL "VULKAN")
        set(model_shader_extension glsl)
    else()
        message(FATAL_ERROR "Unsupported Model shader backend: ${DY_BACKEND_NORMALIZED}")
    endif()
    dy_embed_model_shader("${DY_MODEL_SHADER_SOURCE_DIR}/mesh_vs.${model_shader_extension}"
        vert ModelVertexShader -DRENDERER_ENABLE_SHADOWS=1)
    dy_embed_model_shader("${DY_MODEL_SHADER_SOURCE_DIR}/mesh_vs.${model_shader_extension}"
        vert ModelVertexShaderNoShadows -DRENDERER_ENABLE_SHADOWS=0)
    dy_embed_model_shader("${DY_MODEL_SHADER_SOURCE_DIR}/mesh_shadow_vs.${model_shader_extension}"
        vert ModelShadowShader)
    target_compile_definitions(dy_extend_model PRIVATE DY_MODEL_SHADERS_NATIVE=1)
    dy_embed_model_shader("${DY_MODEL_SHADER_SOURCE_DIR}/mesh_skinning_cs.${model_shader_extension}" comp ModelComputeShader)
    target_compile_definitions(dy_extend_model PRIVATE DY_MODEL_SHADERS_COMPUTE=1)
endif()

add_custom_target(dy_model_shaders DEPENDS ${DY_MODEL_SHADER_HEADERS})
add_dependencies(dy_extend_model dy_model_shaders)
target_include_directories(dy_extend_model PRIVATE "${DY_MODEL_SHADER_GENERATED_DIR}")
