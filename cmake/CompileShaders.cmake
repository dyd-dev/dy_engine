# Compile one program stage from the caller's own source files. The generated
# header contains bytecode and its entry point only; no rendering policy.
include_guard(GLOBAL)

function(dy_compile_shader target name stage)
    cmake_parse_arguments(SHADER "" "GLSL;HLSL;METAL;METAL_ENTRY" "DEFINES;INCLUDE_DIRS" ${ARGN})
    if(NOT stage MATCHES "^(vert|frag)$")
        message(FATAL_ERROR "dy_compile_shader: stage must be vert or frag")
    endif()
    foreach(language IN ITEMS GLSL HLSL METAL)
        if(NOT EXISTS "${SHADER_${language}}")
            message(FATAL_ERROR "Missing ${language} source for ${target}/${name}: ${SHADER_${language}}")
        endif()
    endforeach()
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/shaders/${target}")
    set(header "${directory}/${name}.h")
    set(binary "${directory}/${name}.bin")
    set(embed "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake")
    set(defines)
    foreach(define IN LISTS SHADER_DEFINES)
        list(APPEND defines "-D${define}")
    endforeach()
    set(includes)
    set(dependencies "${SHADER_GLSL}" "${SHADER_HLSL}" "${SHADER_METAL}")
    foreach(include IN LISTS SHADER_INCLUDE_DIRS)
        list(APPEND includes -I "${include}")
        file(GLOB_RECURSE headers CONFIGURE_DEPENDS
            "${include}/*.inc" "${include}/*.glsl" "${include}/*.hlsl" "${include}/*.metal")
        list(APPEND dependencies ${headers})
    endforeach()
    set(entry main)
    if(USE_VULKAN)
        find_program(DY_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
        add_custom_command(OUTPUT "${binary}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
            COMMAND "${DY_GLSLC}" "-fshader-stage=${stage}" ${defines} ${includes} "${SHADER_GLSL}" -o "${binary}"
            DEPENDS ${dependencies} VERBATIM)
    elseif(USE_D3D12)
        find_program(DY_DXC NAMES dxc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
        if(stage STREQUAL "vert")
            set(profile vs_6_0)
        else()
            set(profile ps_6_0)
        endif()
        add_custom_command(OUTPUT "${binary}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
            COMMAND "${DY_DXC}" -T "${profile}" -E "${entry}" ${defines} ${includes} -Fo "${binary}" "${SHADER_HLSL}"
            DEPENDS ${dependencies} VERBATIM)
    elseif(USE_METAL)
        find_program(DY_XCRUN NAMES xcrun REQUIRED)
        if(NOT SHADER_METAL_ENTRY)
            message(FATAL_ERROR "A Metal entry point is required for ${target}/${name}")
        endif()
        set(entry "${SHADER_METAL_ENTRY}")
        add_custom_command(OUTPUT "${binary}"
            BYPRODUCTS "${directory}/${name}.air"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
            COMMAND "${DY_XCRUN}" -sdk macosx metal ${defines} ${includes}
                -c "${SHADER_METAL}" -o "${directory}/${name}.air"
            COMMAND "${DY_XCRUN}" -sdk macosx metallib "${directory}/${name}.air" -o "${binary}"
            DEPENDS ${dependencies} VERBATIM)
    else()
        # Null validates commands; it has no native shader language or pixels.
        file(GENERATE OUTPUT "${header}" CONTENT
            "#pragma once\n#include <cstddef>\nnamespace ShaderData { inline constexpr const void* ${name} = nullptr; inline constexpr std::size_t ${name}Size = 0; inline constexpr const char* ${name}EntryPoint = nullptr; }\n")
        target_include_directories(${target} PRIVATE "${directory}")
        return()
    endif()
    add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${binary}" "-DOUTPUT=${header}" "-DSYMBOL=${name}"
            "-DNAMESPACE=ShaderData" "-DENTRY_POINT=${entry}" -P "${embed}"
        DEPENDS "${binary}" "${embed}" VERBATIM)
    target_sources(${target} PRIVATE "${header}")
    target_include_directories(${target} PRIVATE "${directory}")
endfunction()
