# The same file supplies build functions and embeds compiled binaries via cmake -P.
if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
        message(FATAL_ERROR "INPUT, OUTPUT and SYMBOL are required")
    endif()
    file(READ "${INPUT}" binary HEX)
    if(binary STREQUAL "")
        message(FATAL_ERROR "Cannot embed empty shader: ${INPUT}")
    endif()
    string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1," bytes "${binary}")
    get_filename_component(directory "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${OUTPUT}"
        "#pragma once\n#include <cstddef>\n#include <cstdint>\nnamespace ${NAMESPACE} {\ninline constexpr uint8_t ${SYMBOL}[] = {${bytes}};\ninline constexpr std::size_t ${SYMBOL}Size = sizeof(${SYMBOL});\n")
    if(ENTRY_POINT)
        file(APPEND "${OUTPUT}" "inline constexpr const char* ${SYMBOL}EntryPoint = \"${ENTRY_POINT}\";\n")
    endif()
    file(APPEND "${OUTPUT}" "}\n")
    return()
endif()
include_guard(GLOBAL)

function(dy_embed_shader target name binary symbol namespace entry)
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/shaders/${target}")
    set(header "${directory}/${name}.h")
    set(script "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${binary}" "-DOUTPUT=${header}" "-DSYMBOL=${symbol}"
            "-DNAMESPACE=${namespace}" "-DENTRY_POINT=${entry}" -P "${script}"
        DEPENDS "${binary}" "${script}" VERBATIM)
    target_sources(${target} PRIVATE "${header}")
    target_include_directories(${target} PRIVATE "${directory}")
endfunction()

# LIBRARY collects Metal objects for a subsequent dy_link_shader_library call.
function(dy_compile_shader target name stage)
    # Standalone examples may have different USE_* variables in their parent scope.
    get_target_property(backend dy_engine DY_BACKEND)
    cmake_parse_arguments(SHADER "ANONYMOUS" "GLSL;HLSL;METAL;METAL_ENTRY;SYMBOL;LIBRARY"
        "INCLUDE_DIRS;DEFINES" ${ARGN})
    if(NOT stage MATCHES "^(vert|frag|comp)$")
        message(FATAL_ERROR "dy_compile_shader: stage must be vert, frag or comp")
    endif()
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/shaders/${target}")
    set(binary "${directory}/${name}.bin")
    set(entry main)
    set(symbol "${name}")
    if(SHADER_SYMBOL)
        set(symbol "${SHADER_SYMBOL}")
    endif()
    set(namespace ShaderData)
    if(SHADER_ANONYMOUS)
        set(namespace "")
    endif()
    set(flags)
    set(dependencies)
    foreach(include IN LISTS SHADER_INCLUDE_DIRS)
        list(APPEND flags -I "${include}")
        file(GLOB_RECURSE headers CONFIGURE_DEPENDS
            "${include}/*.inc" "${include}/*.glsl" "${include}/*.hlsl" "${include}/*.metal")
        list(APPEND dependencies ${headers})
    endforeach()
    foreach(define IN LISTS SHADER_DEFINES)
        list(APPEND flags "-D${define}")
    endforeach()
    if(backend STREQUAL "Vulkan")
        set(source "${SHADER_GLSL}")
        find_program(DY_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
        set(compile "${DY_GLSLC}" "-fshader-stage=${stage}" ${flags} "${source}" -o "${binary}")
    elseif(backend STREQUAL "D3D12")
        set(source "${SHADER_HLSL}")
        find_program(DY_DXC NAMES dxc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
        if(stage STREQUAL "vert")
            set(profile vs_6_0)
        elseif(stage STREQUAL "comp")
            set(profile cs_6_0)
        else()
            set(profile ps_6_0)
        endif()
        set(compile "${DY_DXC}" -T "${profile}" -E main ${flags} -Fo "${binary}" "${source}")
    elseif(backend STREQUAL "Metal")
        set(source "${SHADER_METAL}")
        set(entry "${SHADER_METAL_ENTRY}")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "Missing shader source for ${target}/${name}: ${source}")
        endif()
        if(NOT SHADER_ANONYMOUS AND NOT SHADER_METAL_ENTRY)
            message(FATAL_ERROR "A Metal entry point is required for ${target}/${name}")
        endif()
        find_program(DY_XCRUN NAMES xcrun REQUIRED)
        set(air "${directory}/${name}.air")
        set(compile "${DY_XCRUN}" -sdk macosx metal ${flags} -c "${source}" -o "${air}")
        add_custom_command(OUTPUT "${air}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
            COMMAND ${compile} DEPENDS "${source}" ${dependencies} VERBATIM)
        if(SHADER_LIBRARY)
            set_property(TARGET ${target} APPEND PROPERTY "DY_SHADER_${SHADER_LIBRARY}" "${air}")
            return()
        endif()
        set(compile "${DY_XCRUN}" -sdk macosx metallib "${air}" -o "${binary}")
        list(APPEND dependencies "${air}")
    else()
        file(GENERATE OUTPUT "${directory}/${name}.h" CONTENT
            "#pragma once\n#include <cstddef>\nnamespace ${namespace} { inline constexpr const void* ${symbol} = nullptr; inline constexpr std::size_t ${symbol}Size = 0; inline constexpr const char* ${symbol}EntryPoint = nullptr; }\n")
        target_include_directories(${target} PRIVATE "${directory}")
        return()
    endif()
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Missing shader source for ${target}/${name}: ${source}")
    endif()
    add_custom_command(OUTPUT "${binary}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
        COMMAND ${compile} DEPENDS "${source}" ${dependencies} VERBATIM)
    dy_embed_shader(${target} "${name}" "${binary}" "${symbol}" "${namespace}" "${entry}")
endfunction()

function(dy_link_shader_library target name)
    get_target_property(objects ${target} "DY_SHADER_${name}")
    set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/${target}/${name}.metallib")
    find_program(DY_XCRUN NAMES xcrun REQUIRED)
    add_custom_command(OUTPUT "${binary}"
        COMMAND "${DY_XCRUN}" -sdk macosx metallib ${objects} -o "${binary}"
        DEPENDS ${objects} VERBATIM)
    dy_embed_shader(${target} "${name}" "${binary}" "k${name}" "" "")
endfunction()

function(dy_add_engine_shaders target)
    if(NOT USE_D3D12 AND NOT USE_VULKAN AND NOT USE_METAL)
        return()
    endif()
    set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/dyf/Shaders")
    set(stock_library "")
    set(bindless_library "")
    if(USE_METAL)
        set(stock_library StockMetalLibrary)
        set(bindless_library BindlessMetalLibrary)
    endif()
    foreach(shadows IN ITEMS 1 0)
        set(suffix "")
        if(NOT shadows)
            set(suffix NoShadows)
        endif()
        dy_compile_shader(${target} StockVertexShader${suffix} vert
            GLSL "${shader_dir}/mesh_vs.glsl" HLSL "${shader_dir}/mesh_vs.hlsl" METAL "${shader_dir}/mesh_vs.metal"
            ANONYMOUS SYMBOL kStockVertexShader${suffix} LIBRARY "${stock_library}"
            DEFINES RENDERER_ENABLE_SHADOWS=${shadows} RENDERER_VERTEX_ENTRY=vertexShader${suffix})
        dy_compile_shader(${target} StockFragmentShader${suffix} frag
            GLSL "${shader_dir}/mesh_ps.glsl" HLSL "${shader_dir}/mesh_ps.hlsl" METAL "${shader_dir}/mesh_ps.metal"
            ANONYMOUS SYMBOL kStockFragmentShader${suffix} LIBRARY "${stock_library}"
            DEFINES RENDERER_ENABLE_SHADOWS=${shadows} RENDERER_FRAGMENT_ENTRY=fragmentShader${suffix})
        dy_compile_shader(${target} BindlessFragment${suffix} frag
            GLSL "${shader_dir}/mesh_ps.glsl" HLSL "${shader_dir}/mesh_ps.hlsl" METAL "${shader_dir}/mesh_ps.metal"
            ANONYMOUS SYMBOL kBindlessFragment${suffix} LIBRARY "${bindless_library}"
            DEFINES RENDERER_BINDLESS=1 RENDERER_ENABLE_SHADOWS=${shadows} RENDERER_FRAGMENT_ENTRY=bindlessFragment${shadows})
    endforeach()
    dy_compile_shader(${target} StockShadowVertexShader vert
        GLSL "${shader_dir}/mesh_shadow_vs.glsl" HLSL "${shader_dir}/mesh_shadow_vs.hlsl" METAL "${shader_dir}/mesh_shadow_vs.metal"
        ANONYMOUS SYMBOL kStockShadowVertexShader LIBRARY "${stock_library}")
    if(USE_METAL)
        dy_link_shader_library(${target} StockMetalLibrary)
        dy_link_shader_library(${target} BindlessMetalLibrary)
        dy_compile_shader(${target} CanvasLibrary vert METAL "${shader_dir}/canvas.metal" ANONYMOUS SYMBOL kCanvasLibrary)
        dy_compile_shader(${target} ToneMapLibrary vert METAL "${shader_dir}/tone_map.metal" ANONYMOUS SYMBOL kToneMapLibrary)
    else()
        foreach(program IN ITEMS Canvas ToneMap)
            set(source canvas)
            if(program STREQUAL "ToneMap")
                set(source tone_map)
            endif()
            dy_compile_shader(${target} ${program}Vertex vert
                GLSL "${shader_dir}/${source}_vs.glsl" HLSL "${shader_dir}/${source}_vs.hlsl"
                ANONYMOUS SYMBOL k${program}Vertex)
            dy_compile_shader(${target} ${program}Fragment frag
                GLSL "${shader_dir}/${source}_ps.glsl" HLSL "${shader_dir}/${source}_ps.hlsl"
                ANONYMOUS SYMBOL k${program}Fragment)
        endforeach()
    endif()
endfunction()
