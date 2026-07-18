# 예제 공통 셋업 + 셰이더 처리. (cmake/ 모듈이나 메인 CMakeLists 가 아니라 여기서 정의)
# add_subdirectory 이전에 정의해야 하위 예제 스코프에서 보인다.

# 빌드된 셰이더(또는 소스)를 예제 작업 폴더로 배치한다.
#  - Vulkan : glslc 로 .glsl -> .spv
#  - D3D12  : .hlsl 소스 복사(런타임 D3DCompile)
#  - 그 외  : .glsl 소스 복사(Null)
function(dy_example_shaders target)
    set(src_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
    set(out_dir "$<TARGET_FILE_DIR:${target}>/Shaders")
    set(inc "${CMAKE_SOURCE_DIR}/src")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}> "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)

    if(USE_VULKAN)
        find_program(DY_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${DY_GLSLC} -fshader-stage=vert -I "${inc}" "${src_dir}/mesh_vs.glsl" -o "${out_dir}/mesh_vs.spv"
            COMMAND ${DY_GLSLC} -fshader-stage=frag -I "${inc}" "${src_dir}/mesh_ps.glsl" -o "${out_dir}/mesh_ps.spv"
            VERBATIM)
        if(EXISTS "${src_dir}/mesh_shadow_vs.glsl")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${DY_GLSLC} -fshader-stage=vert -I "${inc}" "${src_dir}/mesh_shadow_vs.glsl" -o "${out_dir}/mesh_shadow_vs.spv"
                VERBATIM)
        endif()
        if(EXISTS "${src_dir}/mesh_ps_bindless.glsl")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${DY_GLSLC} -fshader-stage=frag -I "${inc}" "${src_dir}/mesh_ps_bindless.glsl" -o "${out_dir}/mesh_ps_bindless.spv"
                VERBATIM)
        endif()
        if(EXISTS "${src_dir}/tone_map_vs.glsl" AND EXISTS "${src_dir}/tone_map_ps.glsl")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${DY_GLSLC} -fshader-stage=vert -I "${inc}" "${src_dir}/tone_map_vs.glsl" -o "${out_dir}/tone_map_vs.spv"
                COMMAND ${DY_GLSLC} -fshader-stage=frag -I "${inc}" "${src_dir}/tone_map_ps.glsl" -o "${out_dir}/tone_map_ps.spv"
                VERBATIM)
        endif()
    elseif(USE_D3D12)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "${src_dir}/mesh_vs.hlsl" "${out_dir}/mesh_vs.hlsl"
            COMMAND ${CMAKE_COMMAND} -E copy "${src_dir}/mesh_ps.hlsl" "${out_dir}/mesh_ps.hlsl"
            VERBATIM)
        if(EXISTS "${src_dir}/mesh_ps_bindless.hlsl")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy "${src_dir}/mesh_ps_bindless.hlsl" "${out_dir}/mesh_ps_bindless.hlsl"
                VERBATIM)
        endif()
        if(EXISTS "${src_dir}/mesh_shadow_vs.hlsl")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy "${src_dir}/mesh_shadow_vs.hlsl" "${out_dir}/mesh_shadow_vs.hlsl"
                VERBATIM)
        endif()
    else()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "${src_dir}/mesh_vs.glsl" "${out_dir}/mesh_vs.glsl"
            COMMAND ${CMAKE_COMMAND} -E copy "${src_dir}/mesh_ps.glsl" "${out_dir}/mesh_ps.glsl"
            VERBATIM)
    endif()
endfunction()

# 예제 타깃 공통 셋업: 엔진/백엔드 링크 + 인클루드 + 셰이더 디렉터리 정의 + 셰이더 배치.
function(dy_setup_example target)
    target_link_libraries(${target} PRIVATE ${PROJECT_NAME})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(TARGET Backend_D3D12)
        target_link_libraries(${target} PRIVATE Backend_D3D12)
    elseif(TARGET Backend_Metal)
        target_link_libraries(${target} PRIVATE Backend_Metal)
    elseif(TARGET Backend_Vulkan)
        target_link_libraries(${target} PRIVATE Backend_Vulkan)
    elseif(TARGET Backend_Null)
        target_link_libraries(${target} PRIVATE Backend_Null)
    endif()
    target_compile_definitions(${target} PRIVATE DY_SHADER_DIR="./Shaders")
    set_property(TARGET ${target} PROPERTY FOLDER ${target})
    set_property(TARGET ${target} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>")
    dy_example_shaders(${target})
endfunction()

add_subdirectory(01_HelloWindow)
add_subdirectory(02_HelloRenderer)
add_subdirectory(03_Cube)
add_subdirectory(04_TexturedCube)
add_subdirectory(05_LoadModel)
add_subdirectory(06_ShadowCube)
add_subdirectory(07_RenderPath)
add_subdirectory(08_LightingLab)
