if(USE_D3D12)
    set(DY_BACKEND_NORMALIZED D3D12)
elseif(USE_METAL)
    set(DY_BACKEND_NORMALIZED METAL)
elseif(USE_VULKAN)
    set(DY_BACKEND_NORMALIZED VULKAN)
else()
    set(DY_BACKEND_NORMALIZED NULL)
endif()

set(DY_STOCK_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/dyf/Shaders")
set(DY_STOCK_SHADER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/stock_shaders")
set(DY_STOCK_SHADER_EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinary.cmake")
set(DY_STOCK_SHADER_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/dyf/Shaders")
set(DY_STOCK_SHADER_SHARED_SOURCES
    "${DY_STOCK_SHADER_SHARED_DIR}/shadow_filter.glsl"
    "${DY_STOCK_SHADER_SHARED_DIR}/shadow_filter.hlsl"
    "${DY_STOCK_SHADER_SHARED_DIR}/shadow_filter.metal")

# Every stock program must ship all three native representations, even when the
# current host can only run one of the platform compilers.
set(DY_STOCK_SHADER_NATIVE_SOURCES)
foreach(language IN ITEMS glsl hlsl metal)
    set(programs mesh_vs mesh_ps mesh_shadow_vs mesh_shadow_ps
        gpu_visibility gpu_hiz)
    if(language STREQUAL "metal")
        list(APPEND programs canvas tone_map presentation_copy environment_depth)
    else()
        list(APPEND programs canvas_vs canvas_ps tone_map_vs tone_map_ps presentation_copy_ps environment_depth_ps)
    endif()
    foreach(program IN LISTS programs)
        set(source "${DY_STOCK_SHADER_SOURCE_DIR}/${program}.${language}")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "Missing ${language} stock shader: ${source}")
        endif()
        list(APPEND DY_STOCK_SHADER_NATIVE_SOURCES "${source}")
    endforeach()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${DY_STOCK_SHADER_NATIVE_SOURCES} ${DY_STOCK_SHADER_SHARED_SOURCES})

# LTC 표는 셰이더 바이트코드와 독립된 공용 데이터다.
set(DY_LTC_TABLE_HEADERS)
foreach(table IN ITEMS Matrix Amplitude)
    if(table STREQUAL "Matrix")
        set(ltc_table "${CMAKE_CURRENT_SOURCE_DIR}/src/dyf/Assets/ltc/ltc_1.dds")
    else()
        set(ltc_table "${CMAKE_CURRENT_SOURCE_DIR}/src/dyf/Assets/ltc/ltc_2.dds")
    endif()
    set(header "${DY_STOCK_SHADER_GENERATED_DIR}/AreaLtc${table}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${ltc_table}" "-DOUTPUT=${header}" "-DSYMBOL=kAreaLtc${table}"
            -P "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        DEPENDS "${ltc_table}" "${DY_STOCK_SHADER_EMBED_SCRIPT}" VERBATIM)
    list(APPEND DY_LTC_TABLE_HEADERS "${header}")
endforeach()
add_custom_target(dy_ltc_assets DEPENDS ${DY_LTC_TABLE_HEADERS})
add_dependencies(dy_engine dy_ltc_assets)
target_include_directories(dy_engine PRIVATE "${DY_STOCK_SHADER_GENERATED_DIR}")

# Null has no native shader bytecode. Do not silently select GLSL for this backend.
if(DY_BACKEND_NORMALIZED STREQUAL "NULL")
    return()
endif()

set(DY_STOCK_SHADER_HEADERS)

function(dy_embed_stock_shader source stage name symbol)
    set(binary "${DY_STOCK_SHADER_GENERATED_DIR}/${name}.bin")
    set(shader_defines ${ARGN})

    if(DY_BACKEND_NORMALIZED STREQUAL "D3D12")
        if(NOT DY_DXC)
            find_program(DY_DXC NAMES dxc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
            if(NOT DY_DXC)
                message(FATAL_ERROR "dxc is required to build the D3D12 stock shaders")
            endif()
        endif()
        if(stage STREQUAL "vert")
            set(profile vs_6_0)
        elseif(stage STREQUAL "comp")
            set(profile cs_6_0)
        else()
            set(profile ps_6_0)
        endif()
        add_custom_command(
            OUTPUT "${binary}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
            COMMAND "${DY_DXC}" -T "${profile}" -E main -I "${DY_STOCK_SHADER_SHARED_DIR}" ${shader_defines} -Fo "${binary}" "${source}"
            DEPENDS "${source}" ${DY_STOCK_SHADER_SHARED_SOURCES}
            VERBATIM)
    elseif(DY_BACKEND_NORMALIZED STREQUAL "VULKAN")
        if(NOT DY_GLSLC)
            find_program(DY_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
            if(NOT DY_GLSLC)
                message(FATAL_ERROR "glslc is required to build the Vulkan stock shaders")
            endif()
        endif()
        add_custom_command(
            OUTPUT "${binary}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
            COMMAND "${DY_GLSLC}" "-fshader-stage=${stage}" "-I${DY_STOCK_SHADER_SHARED_DIR}" ${shader_defines} "${source}" -o "${binary}"
            DEPENDS "${source}" ${DY_STOCK_SHADER_SHARED_SOURCES}
            VERBATIM)
    else()
        message(FATAL_ERROR "No shader compiler configured for ${DY_BACKEND_NORMALIZED}")
    endif()

    set(header "${DY_STOCK_SHADER_GENERATED_DIR}/${name}.h")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${binary}" "-DOUTPUT=${header}" "-DSYMBOL=${symbol}" -P "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        DEPENDS "${binary}" "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        VERBATIM)
    set(DY_STOCK_SHADER_HEADERS "${DY_STOCK_SHADER_HEADERS};${header}" PARENT_SCOPE)
endfunction()

# 진입점이 다른 셰이더는 같은 라이브러리에 묶을 수 있다.
function(dy_embed_stock_metal_library name symbol)
    cmake_parse_arguments(DY_METAL_EMBED "" "" "INVARIANT_PROGRAMS" ${ARGN})
    set(air_files)
    set(programs)
    set(shader_defines)
    foreach(argument IN LISTS DY_METAL_EMBED_UNPARSED_ARGUMENTS)
        if(argument MATCHES "^-D")
            list(APPEND shader_defines "${argument}")
        else()
            list(APPEND programs "${argument}")
        endif()
    endforeach()
    foreach(program IN LISTS programs)
        set(source "${DY_STOCK_SHADER_SOURCE_DIR}/${program}.metal")
        set(air "${DY_STOCK_SHADER_GENERATED_DIR}/${name}_${program}.air")
        set(position_options)
        if(program IN_LIST DY_METAL_EMBED_INVARIANT_PROGRAMS)
            # 호출자가 지정한 정점 단계의 위치 연산을 다른 패스와 동일하게 유지한다.
            set(position_options -fpreserve-invariance)
        endif()
        add_custom_command(OUTPUT "${air}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
            COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${position_options} ${shader_defines} -I "${DY_STOCK_SHADER_SHARED_DIR}" -c "${source}" -o "${air}"
            DEPENDS "${source}" ${DY_STOCK_SHADER_SHARED_SOURCES} VERBATIM)
        list(APPEND air_files "${air}")
    endforeach()
    set(library "${DY_STOCK_SHADER_GENERATED_DIR}/${name}.metallib")
    add_custom_command(OUTPUT "${library}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metallib ${air_files} -o "${library}"
        DEPENDS ${air_files} VERBATIM)
    set(header "${DY_STOCK_SHADER_GENERATED_DIR}/${name}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${library}" "-DOUTPUT=${header}" "-DSYMBOL=${symbol}"
            -P "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        DEPENDS "${library}" "${DY_STOCK_SHADER_EMBED_SCRIPT}" VERBATIM)
    set(DY_STOCK_SHADER_HEADERS "${DY_STOCK_SHADER_HEADERS};${header}" PARENT_SCOPE)
endfunction()

if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    include(MetalToolchain)

    set(metal_air_files)
    set(source "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_shadow_vs.metal")
    set(air "${DY_STOCK_SHADER_GENERATED_DIR}/mesh_shadow_vs.air")
    add_custom_command(
        OUTPUT "${air}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal -c "${source}" -o "${air}"
        DEPENDS "${source}"
        VERBATIM)
    list(APPEND metal_air_files "${air}")

    set(shadow_fragment_source "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_shadow_ps.metal")
    set(shadow_fragment_air "${DY_STOCK_SHADER_GENERATED_DIR}/mesh_shadow_ps.air")
    add_custom_command(OUTPUT "${shadow_fragment_air}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal -c "${shadow_fragment_source}" -o "${shadow_fragment_air}"
        DEPENDS "${shadow_fragment_source}" VERBATIM)
    list(APPEND metal_air_files "${shadow_fragment_air}")

    set(air "${DY_STOCK_SHADER_GENERATED_DIR}/mesh_shadow_instanced_vs.air")
    add_custom_command(
        OUTPUT "${air}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal
            -DRENDERER_INSTANCING=1
            -DRENDERER_SHADOW_VERTEX_ENTRY=instancedShadowVertexShader
            -c "${source}" -o "${air}"
        DEPENDS "${source}"
        VERBATIM)
    list(APPEND metal_air_files "${air}")

    foreach(enable_shadows IN ITEMS 1 0)
        if(enable_shadows)
            set(shader mesh_vs)
            set(vertex_entry vertexShader)
        else()
            set(shader mesh_vs_no_shadows)
            set(vertex_entry vertexShaderNoShadows)
        endif()
        set(source "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_vs.metal")
        set(air "${DY_STOCK_SHADER_GENERATED_DIR}/${shader}.air")
        add_custom_command(
            OUTPUT "${air}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
            COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal
                "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}"
                "-DRENDERER_VERTEX_ENTRY=${vertex_entry}"
                -fpreserve-invariance -c "${source}" -o "${air}"
            DEPENDS "${source}"
            VERBATIM)
        list(APPEND metal_air_files "${air}")
    endforeach()

    foreach(enable_shadows IN ITEMS 1 0)
        if(enable_shadows)
            set(vertex_entry instancedVertexShader)
        else()
            set(vertex_entry instancedVertexShaderNoShadows)
        endif()
        set(source "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_vs.metal")
        set(air "${DY_STOCK_SHADER_GENERATED_DIR}/mesh_instanced_${enable_shadows}.air")
        add_custom_command(OUTPUT "${air}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
            COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal
                "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}" -DRENDERER_INSTANCING=1
                "-DRENDERER_VERTEX_ENTRY=${vertex_entry}" -fpreserve-invariance -c "${source}" -o "${air}"
            DEPENDS "${source}" VERBATIM)
        list(APPEND metal_air_files "${air}")
    endforeach()

    foreach(enable_shadows IN ITEMS 1 0)
        if(enable_shadows)
            set(shader mesh_ps)
            set(fragment_entry fragmentShader)
        else()
            set(shader mesh_ps_no_shadows)
            set(fragment_entry fragmentShaderNoShadows)
        endif()
        set(source "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.metal")
        set(air "${DY_STOCK_SHADER_GENERATED_DIR}/${shader}.air")
        add_custom_command(
            OUTPUT "${air}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
            COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal
                "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}"
                "-DRENDERER_FRAGMENT_ENTRY=${fragment_entry}"
                -c "${source}" -o "${air}"
            DEPENDS "${source}" "${DY_STOCK_SHADER_SHARED_DIR}/shadow_filter.metal"
            VERBATIM)
        list(APPEND metal_air_files "${air}")
    endforeach()

    set(metal_library "${DY_STOCK_SHADER_GENERATED_DIR}/StockShaders.metallib")
    add_custom_command(
        OUTPUT "${metal_library}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metallib ${metal_air_files} -o "${metal_library}"
        DEPENDS ${metal_air_files}
        VERBATIM)
    set(metal_header "${DY_STOCK_SHADER_GENERATED_DIR}/StockMetalLibrary.h")
    add_custom_command(
        OUTPUT "${metal_header}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${metal_library}" "-DOUTPUT=${metal_header}" -DSYMBOL=kStockMetalLibrary -P "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        DEPENDS "${metal_library}" "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        VERBATIM)
    list(APPEND DY_STOCK_SHADER_HEADERS "${metal_header}")
else()
    if(DY_BACKEND_NORMALIZED STREQUAL "D3D12")
        set(shader_extension hlsl)
    elseif(DY_BACKEND_NORMALIZED STREQUAL "VULKAN")
        set(shader_extension glsl)
    else()
        message(FATAL_ERROR "Unsupported native shader backend: ${DY_BACKEND_NORMALIZED}")
    endif()
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_vs.${shader_extension}" vert StockInstancedVertexShader kStockInstancedVertexShader -DRENDERER_ENABLE_SHADOWS=1 -DRENDERER_INSTANCING=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_vs.${shader_extension}" vert StockInstancedVertexShaderNoShadows kStockInstancedVertexShaderNoShadows -DRENDERER_ENABLE_SHADOWS=0 -DRENDERER_INSTANCING=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_vs.${shader_extension}" vert StockVertexShader kStockVertexShader -DRENDERER_ENABLE_SHADOWS=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_vs.${shader_extension}" vert StockVertexShaderNoShadows kStockVertexShaderNoShadows -DRENDERER_ENABLE_SHADOWS=0)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag StockFragmentShader kStockFragmentShader -DRENDERER_ENABLE_SHADOWS=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag StockFragmentShaderNoShadows kStockFragmentShaderNoShadows -DRENDERER_ENABLE_SHADOWS=0)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_shadow_vs.${shader_extension}" vert StockShadowVertexShader kStockShadowVertexShader)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_shadow_vs.${shader_extension}" vert StockInstancedShadowVertexShader kStockInstancedShadowVertexShader -DRENDERER_INSTANCING=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_shadow_ps.${shader_extension}" frag StockShadowFragmentShader kStockShadowFragmentShader)
endif()

if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    set(canvas_air "${DY_STOCK_SHADER_GENERATED_DIR}/Canvas.air")
    set(canvas_library "${DY_STOCK_SHADER_GENERATED_DIR}/Canvas.metallib")
    set(canvas_header "${DY_STOCK_SHADER_GENERATED_DIR}/CanvasLibrary.h")
    add_custom_command(OUTPUT "${canvas_header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal -c "${DY_STOCK_SHADER_SOURCE_DIR}/canvas.metal" -o "${canvas_air}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metallib "${canvas_air}" -o "${canvas_library}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${canvas_library}" "-DOUTPUT=${canvas_header}" -DSYMBOL=kCanvasLibrary -P "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        DEPENDS "${DY_STOCK_SHADER_SOURCE_DIR}/canvas.metal" "${DY_STOCK_SHADER_EMBED_SCRIPT}" VERBATIM)
    list(APPEND DY_STOCK_SHADER_HEADERS "${canvas_header}")
else()
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/canvas_vs.${shader_extension}" vert CanvasVertex kCanvasVertex)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/canvas_ps.${shader_extension}" frag CanvasFragment kCanvasFragment)
endif()

if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    set(tone_map_air "${DY_STOCK_SHADER_GENERATED_DIR}/ToneMap.air")
    set(tone_map_library "${DY_STOCK_SHADER_GENERATED_DIR}/ToneMap.metallib")
    set(tone_map_header "${DY_STOCK_SHADER_GENERATED_DIR}/ToneMapLibrary.h")
    add_custom_command(OUTPUT "${tone_map_header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal -c "${DY_STOCK_SHADER_SOURCE_DIR}/tone_map.metal" -o "${tone_map_air}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metallib "${tone_map_air}" -o "${tone_map_library}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${tone_map_library}" "-DOUTPUT=${tone_map_header}" -DSYMBOL=kToneMapLibrary -P "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        DEPENDS "${DY_STOCK_SHADER_SOURCE_DIR}/tone_map.metal" "${DY_STOCK_SHADER_EMBED_SCRIPT}" VERBATIM)
    list(APPEND DY_STOCK_SHADER_HEADERS "${tone_map_header}")
else()
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/tone_map_vs.${shader_extension}" vert ToneMapVertex kToneMapVertex)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/tone_map_ps.${shader_extension}" frag ToneMapFragment kToneMapFragment)
endif()

if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    dy_embed_stock_metal_library(PresentationCopyLibrary kPresentationCopyLibrary presentation_copy)
    dy_embed_stock_metal_library(EnvironmentDepthLibrary kEnvironmentDepthLibrary environment_depth)
else()
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/presentation_copy_ps.${shader_extension}" frag PresentationCopyFragment kPresentationCopyFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/environment_depth_ps.${shader_extension}" frag EnvironmentDepthFragment kEnvironmentDepthFragment)
endif()


if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    set(bindless_air_files)
    foreach(enable_shadows IN ITEMS 1 0)
        set(air "${DY_STOCK_SHADER_GENERATED_DIR}/Bindless${enable_shadows}.air")
        add_custom_command(OUTPUT "${air}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
            COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal
                -DRENDERER_BINDLESS=1 "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}"
                "-DRENDERER_FRAGMENT_ENTRY=bindlessFragment${enable_shadows}" -c "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.metal" -o "${air}"
            DEPENDS "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.metal" "${DY_STOCK_SHADER_SHARED_DIR}/shadow_filter.metal" VERBATIM)
        list(APPEND bindless_air_files "${air}")
    endforeach()
    set(library "${DY_STOCK_SHADER_GENERATED_DIR}/Bindless.metallib")
    set(header "${DY_STOCK_SHADER_GENERATED_DIR}/BindlessMetalLibrary.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metallib ${bindless_air_files} -o "${library}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${library}" "-DOUTPUT=${header}" -DSYMBOL=kBindlessMetalLibrary -P "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        DEPENDS ${bindless_air_files} "${DY_STOCK_SHADER_EMBED_SCRIPT}" VERBATIM)
    list(APPEND DY_STOCK_SHADER_HEADERS "${header}")
else()
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag BindlessFragment kBindlessFragment -DRENDERER_BINDLESS=1 -DRENDERER_ENABLE_SHADOWS=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag BindlessFragmentNoShadows kBindlessFragmentNoShadows -DRENDERER_BINDLESS=1 -DRENDERER_ENABLE_SHADOWS=0)
endif()

if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    dy_embed_stock_metal_library(GpuVisibilityCompute kGpuVisibilityCompute gpu_visibility)
    dy_embed_stock_metal_library(GpuOcclusionCompute kGpuOcclusionCompute gpu_visibility -DGPU_VISIBILITY_OCCLUSION=1)
    dy_embed_stock_metal_library(GpuHiZCompute kGpuHiZCompute gpu_hiz)
else()
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/gpu_visibility.${shader_extension}" comp GpuVisibilityCompute kGpuVisibilityCompute)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/gpu_visibility.${shader_extension}" comp GpuOcclusionCompute kGpuOcclusionCompute -DGPU_VISIBILITY_OCCLUSION=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/gpu_hiz.${shader_extension}" comp GpuHiZCompute kGpuHiZCompute)
endif()

add_custom_target(dy_stock_shaders DEPENDS ${DY_STOCK_SHADER_HEADERS})
add_dependencies(dy_engine dy_stock_shaders)
target_include_directories(dy_engine PRIVATE "${DY_STOCK_SHADER_GENERATED_DIR}")
