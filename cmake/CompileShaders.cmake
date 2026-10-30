# Script mode embeds binary data; normal includes define shader build helpers.
if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
        message(FATAL_ERROR "INPUT, OUTPUT and SYMBOL are required")
    endif()

    file(READ "${INPUT}" binary HEX)
    if(binary STREQUAL "")
        message(FATAL_ERROR "Cannot embed empty file: ${INPUT}")
    endif()

    string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1," bytes "${binary}")
    get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_directory}")
    if(NOT DEFINED NAMESPACE)
        set(NAMESPACE "")
    endif()
    file(WRITE "${OUTPUT}"
        "#pragma once\n#include <cstddef>\n#include <cstdint>\n\nnamespace ${NAMESPACE}\n{\ninline constexpr uint8_t ${SYMBOL}[] = {${bytes}};\ninline constexpr std::size_t ${SYMBOL}Size = sizeof(${SYMBOL});\n}\n")
    if(DEFINED ENTRY_POINT)
        file(APPEND "${OUTPUT}"
            "namespace ${NAMESPACE} { inline constexpr const char* ${SYMBOL}EntryPoint = \"${ENTRY_POINT}\"; }\n")
    endif()
    return()
endif()

# Native shader toolchains and embedded shader headers.
include_guard(GLOBAL)

# Configure only the selected backend's host tools, in the caller's scope.
macro(dy_configure_shader_toolchain)
    if(USE_D3D12 OR USE_VULKAN)
        set(dy_shader_tool_hints)
        if(DEFINED ENV{VULKAN_SDK})
            list(APPEND dy_shader_tool_hints "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
        endif()
        if(ANDROID AND ANDROID_NDK)
            if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
                set(dy_ndk_host windows-x86_64)
            elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
                set(dy_ndk_host darwin-x86_64)
            else()
                set(dy_ndk_host linux-x86_64)
            endif()
            list(APPEND dy_shader_tool_hints
                "${ANDROID_NDK}/shader-tools/${dy_ndk_host}"
                "${ANDROID_NDK}/shader-tools")
        endif()
        if(USE_D3D12 AND NOT DY_DXC)
            find_program(DY_DXC NAMES dxc HINTS ${dy_shader_tool_hints}
                NO_CMAKE_FIND_ROOT_PATH REQUIRED)
        elseif(USE_VULKAN AND NOT DY_GLSLC)
            find_program(DY_GLSLC NAMES glslc HINTS ${dy_shader_tool_hints}
                NO_CMAKE_FIND_ROOT_PATH REQUIRED)
        endif()
    elseif(USE_METAL AND NOT DEFINED DY_METAL_XCRUN_ARGS)
        # C++ 빌드 SDK와 셰이더 실행 플랫폼을 맞춘다. macOS용 metallib를 iOS에 재사용하지 않는다.

        find_program(DY_XCRUN NAMES xcrun NO_CMAKE_FIND_ROOT_PATH REQUIRED)
        set(metal_sdk macosx)
        set(DY_METAL_COMPILE_ARGS)
        if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(metal_sdk iphoneos)
            set(metal_target "air64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
            if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
                set(metal_sdk iphonesimulator)
                string(APPEND metal_target "-simulator")
            endif()
            # SDK 검색 경로와 별도로 AIR 바이너리의 실행 플랫폼도 지정한다.
            set(DY_METAL_COMPILE_ARGS -target "${metal_target}")
        endif()
        set(DY_METAL_XCRUN_ARGS -sdk "${metal_sdk}")

        # 설치된 Metal 도구 체인을 명시적으로 선택하면서 위에서 정한 SDK도 유지한다.
        find_program(DY_XCODEBUILD NAMES xcodebuild NO_CMAKE_FIND_ROOT_PATH)
        if(DY_XCODEBUILD)
            execute_process(
                COMMAND "${DY_XCODEBUILD}" -showComponent MetalToolchain
                RESULT_VARIABLE metal_component_result
                OUTPUT_VARIABLE metal_component_info
                ERROR_QUIET)
            if(metal_component_result EQUAL 0 AND
               metal_component_info MATCHES "Toolchain Identifier:[ \t]*([^\r\n]+)")
                string(STRIP "${CMAKE_MATCH_1}" metal_toolchain_identifier)
                set(DY_METAL_XCRUN_ARGS --toolchain "${metal_toolchain_identifier}" -sdk "${metal_sdk}")
            endif()
        endif()

        execute_process(
            COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal --version
            RESULT_VARIABLE metal_compiler_result
            OUTPUT_QUIET
            ERROR_VARIABLE metal_compiler_error)
        if(NOT metal_compiler_result EQUAL 0)
            string(STRIP "${metal_compiler_error}" metal_compiler_error)
            message(FATAL_ERROR "Metal compiler is unavailable: ${metal_compiler_error}")
        endif()
    endif()
endmacro()

function(dy_compile_shader target name stage)
    cmake_parse_arguments(SHADER "BINARY_ONLY" "GLSL;HLSL;METAL;METAL_ENTRY;OUTPUT" "INCLUDE_DIRS" ${ARGN})
    if(NOT stage MATCHES "^(vert|hull|domain|frag|comp|mesh)$")
        message(FATAL_ERROR "dy_compile_shader: stage must be vert, hull, domain, frag, comp or mesh")
    endif()
    if(SHADER_BINARY_ONLY AND NOT USE_VULKAN AND NOT USE_D3D12 AND NOT USE_METAL)
        return()
    endif()
    foreach(language IN ITEMS GLSL HLSL METAL)
        if(NOT EXISTS "${SHADER_${language}}")
            message(FATAL_ERROR "Missing ${language} source for ${target}/${name}: ${SHADER_${language}}")
        endif()
    endforeach()
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/shaders/${target}")
    if(CMAKE_CONFIGURATION_TYPES)
        string(APPEND directory "/$<CONFIG>")
    endif()
    set(header "${directory}/${name}.h")
    set(binary "${directory}/${name}.bin")
    if(SHADER_OUTPUT)
        set(binary "${SHADER_OUTPUT}")
        get_filename_component(directory "${binary}" DIRECTORY)
    endif()
    set(embed "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CompileShaders.cmake")
    set(includes)
    set(dependencies "${SHADER_GLSL}" "${SHADER_HLSL}" "${SHADER_METAL}")
    foreach(include IN LISTS SHADER_INCLUDE_DIRS)
        list(APPEND includes -I "${include}")
        file(GLOB_RECURSE headers CONFIGURE_DEPENDS
            "${include}/*.inc" "${include}/*.h" "${include}/*.glsl" "${include}/*.hlsl" "${include}/*.metal")
        list(APPEND dependencies ${headers})
    endforeach()
    set(entry main)
    dy_configure_shader_toolchain()
    if(USE_VULKAN)
        set(glsl_stage "${stage}")
        if(stage STREQUAL "hull")
            set(glsl_stage tesc)
        elseif(stage STREQUAL "domain")
            set(glsl_stage tese)
        endif()
        set(glslc_extra_args)
        if(stage STREQUAL "mesh")
            set(glslc_extra_args "--target-env=vulkan1.3")
        endif()
        set(depfile)
        if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles" OR NOT CMAKE_VERSION VERSION_LESS "3.21")
            set(depfile DEPFILE "${binary}.d")
        endif()
        add_custom_command(OUTPUT "${binary}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
            COMMAND "${DY_GLSLC}" "$<IF:$<CONFIG:Debug>,-O0,-O>" "-fshader-stage=${glsl_stage}" ${glslc_extra_args} ${includes}
                -MD -MF "${binary}.d" "${SHADER_GLSL}" -o "${binary}"
            DEPENDS ${dependencies} ${depfile} VERBATIM)
    elseif(USE_D3D12)
        if(stage STREQUAL "vert")
            set(profile vs_6_0)
        elseif(stage STREQUAL "hull")
            set(profile hs_6_0)
        elseif(stage STREQUAL "domain")
            set(profile ds_6_0)
        elseif(stage STREQUAL "comp")
            set(profile cs_6_0)
        elseif(stage STREQUAL "mesh")
            set(profile ms_6_5)
        else()
            set(profile ps_6_0)
        endif()
        add_custom_command(OUTPUT "${binary}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
            COMMAND "${DY_DXC}" -T "${profile}" -E "${entry}" ${includes} -Fo "${binary}" "${SHADER_HLSL}"
            DEPENDS ${dependencies} VERBATIM)
    elseif(USE_METAL)
        # 대상 SDK의 AIR → metallib로 빌드한다. 아래 공통 단계에서 C++ 헤더로 내장한다.
        if(NOT SHADER_METAL_ENTRY)
            message(FATAL_ERROR "A Metal entry point is required for ${target}/${name}")
        endif()
        set(entry "${SHADER_METAL_ENTRY}")
        add_custom_command(OUTPUT "${binary}"
            BYPRODUCTS "${directory}/${name}.air"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
            COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS} ${includes}
                -c "${SHADER_METAL}" -o "${directory}/${name}.air"
            COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metallib "${directory}/${name}.air" -o "${binary}"
            DEPENDS ${dependencies} VERBATIM)
    else()
        # Null validates commands; it has no native shader language or pixels.
        file(GENERATE OUTPUT "${header}" CONTENT
            "#pragma once\n#include <cstddef>\nnamespace ShaderData { inline constexpr const void* ${name} = nullptr; inline constexpr std::size_t ${name}Size = 0; inline constexpr const char* ${name}EntryPoint = nullptr; }\n")
        target_include_directories(${target} PRIVATE "${directory}")
        return()
    endif()
    if(SHADER_BINARY_ONLY)
        return()
    endif()
    add_custom_command(OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${binary}" "-DOUTPUT=${header}" "-DSYMBOL=${name}"
            "-DNAMESPACE=ShaderData" "-DENTRY_POINT=${entry}" -P "${embed}"
        DEPENDS "${binary}" "${embed}" VERBATIM)
    target_sources(${target} PRIVATE "${header}")
    target_include_directories(${target} PRIVATE "${directory}")
endfunction()

# Examples use the same native compiler commands without embedding byte arrays.
# An imported engine does not expose its backend: no selection leaves existing
# source/precompiled assets alone instead of guessing the archive's contents.
function(dy_example_shader target stage source destination)
    if(NOT USE_VULKAN AND NOT USE_D3D12 AND NOT USE_METAL)
        return()
    endif()
    cmake_parse_arguments(SHADER "" "HLSL;METAL;METAL_ENTRY" "" ${ARGN})
    foreach(path IN ITEMS "${source}" "${destination}" "${SHADER_HLSL}" "${SHADER_METAL}")
        if(NOT path MATCHES "^Shaders/[A-Za-z0-9_./-]+$" OR path MATCHES "(^|/)[.][.]?(/|$)")
            message(FATAL_ERROR "${target}: shader paths must remain inside Shaders")
        endif()
    endforeach()
    get_filename_component(name "${destination}" NAME_WE)
    set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/$<CONFIG>/${target}/${name}.bin")
    # One Metal source can contain several stage entry points. Compile its
    # library once; each declared stage still gets its expected asset filename.
    if(USE_METAL)
        string(SHA256 library_name "${SHADER_METAL}")
        set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/$<CONFIG>/${target}/${library_name}.bin")
    endif()
    get_target_property(compiled "${target}" DY_EXAMPLE_COMPILED_SHADERS)
    if(NOT binary IN_LIST compiled)
        dy_compile_shader("${target}" "${name}" "${stage}" BINARY_ONLY OUTPUT "${binary}"
            GLSL "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            HLSL "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_HLSL}"
            METAL "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_METAL}"
            METAL_ENTRY "${SHADER_METAL_ENTRY}"
            INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
        set_property(TARGET "${target}" APPEND PROPERTY DY_EXAMPLE_COMPILED_SHADERS "${binary}")
    endif()
    if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set_property(TARGET "${target}" APPEND PROPERTY DY_EXAMPLE_SHADER_ASSETS
            "${binary}|${destination}")
        set_property(TARGET "${target}" APPEND PROPERTY DY_EXAMPLE_ASSET_FILES
            "${target}/${destination}")
    else()
        get_filename_component(output_directory "${destination}" DIRECTORY)
        add_custom_target("${target}_${name}_shader" ALL
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/${output_directory}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${binary}" "$<TARGET_FILE_DIR:${target}>/${destination}"
            DEPENDS "${binary}" VERBATIM)
        add_dependencies("${target}" "${target}_${name}_shader")
    endif()
endfunction()

# Build in the caller's directory scope, shared with shader extensions.
macro(dy_build_stock_shaders)
    dy_configure_shader_toolchain()
    set(DY_STOCK_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/dyf/Shaders")
    set(DY_STOCK_SHADER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/stock_shaders")
    if(CMAKE_CONFIGURATION_TYPES)
        string(APPEND DY_STOCK_SHADER_GENERATED_DIR "/$<CONFIG>")
    endif()
    set(DY_STOCK_SHADER_EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShaders.cmake")
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

    # Null has no native shader bytecode.
    if(NOT DY_BACKEND_NORMALIZED STREQUAL "NULL")
        set(DY_STOCK_SHADER_HEADERS)

        function(dy_embed_stock_shader source stage name symbol)
            set(binary "${DY_STOCK_SHADER_GENERATED_DIR}/${name}.bin")
            cmake_parse_arguments(PARSE_ARGV 4 DY_STOCK_EMBED "" "" "")
            set(shader_defines ${DY_STOCK_EMBED_UNPARSED_ARGUMENTS})

            if(DY_BACKEND_NORMALIZED STREQUAL "D3D12")
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
                add_custom_command(
                    OUTPUT "${binary}"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
                    COMMAND "${DY_GLSLC}" "$<IF:$<CONFIG:Debug>,-O0,-O>" "-fshader-stage=${stage}" "-I${DY_STOCK_SHADER_SHARED_DIR}" ${shader_defines} "${source}" -o "${binary}"
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
            cmake_parse_arguments(PARSE_ARGV 2 DY_METAL_EMBED "" "" "INVARIANT_PROGRAMS")
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
                    COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS} ${position_options} ${shader_defines} -I "${DY_STOCK_SHADER_SHARED_DIR}" -c "${source}" -o "${air}"
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
            set(metal_air_files)
            set(source "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_shadow_vs.metal")
            set(air "${DY_STOCK_SHADER_GENERATED_DIR}/mesh_shadow_vs.air")
            add_custom_command(
                OUTPUT "${air}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
                COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS} -c "${source}" -o "${air}"
                DEPENDS "${source}"
                VERBATIM)
            list(APPEND metal_air_files "${air}")

            set(shadow_fragment_source "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_shadow_ps.metal")
            set(shadow_fragment_air "${DY_STOCK_SHADER_GENERATED_DIR}/mesh_shadow_ps.air")
            add_custom_command(OUTPUT "${shadow_fragment_air}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
                COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS} -c "${shadow_fragment_source}" -o "${shadow_fragment_air}"
                DEPENDS "${shadow_fragment_source}" VERBATIM)
            list(APPEND metal_air_files "${shadow_fragment_air}")

            set(air "${DY_STOCK_SHADER_GENERATED_DIR}/mesh_shadow_instanced_vs.air")
            add_custom_command(
                OUTPUT "${air}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
                COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS}
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
                    COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS}
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
                    COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS}
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
                    COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS}
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
                BYPRODUCTS "${canvas_air}" "${canvas_library}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
                COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS} -c "${DY_STOCK_SHADER_SOURCE_DIR}/canvas.metal" -o "${canvas_air}"
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
                BYPRODUCTS "${tone_map_air}" "${tone_map_library}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
                COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS} -c "${DY_STOCK_SHADER_SOURCE_DIR}/tone_map.metal" -o "${tone_map_air}"
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
                    COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metal ${DY_METAL_COMPILE_ARGS}
                        -DRENDERER_BINDLESS=1 "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}"
                        "-DRENDERER_FRAGMENT_ENTRY=bindlessFragment${enable_shadows}" -c "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.metal" -o "${air}"
                    DEPENDS "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.metal" "${DY_STOCK_SHADER_SHARED_DIR}/shadow_filter.metal" VERBATIM)
                list(APPEND bindless_air_files "${air}")
            endforeach()
            set(library "${DY_STOCK_SHADER_GENERATED_DIR}/Bindless.metallib")
            set(header "${DY_STOCK_SHADER_GENERATED_DIR}/BindlessMetalLibrary.h")
            add_custom_command(OUTPUT "${header}"
                BYPRODUCTS "${library}"
                COMMAND "${DY_XCRUN}" ${DY_METAL_XCRUN_ARGS} metallib ${bindless_air_files} -o "${library}"
                COMMAND "${CMAKE_COMMAND}" "-DINPUT=${library}" "-DOUTPUT=${header}" -DSYMBOL=kBindlessMetalLibrary -P "${DY_STOCK_SHADER_EMBED_SCRIPT}"
                DEPENDS ${bindless_air_files} "${DY_STOCK_SHADER_EMBED_SCRIPT}" VERBATIM)
            list(APPEND DY_STOCK_SHADER_HEADERS "${header}")
        else()
            dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag BindlessFragment kBindlessFragment -DRENDERER_BINDLESS=1 -DRENDERER_ENABLE_SHADOWS=1)
            dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag BindlessFragmentNoShadows kBindlessFragmentNoShadows -DRENDERER_BINDLESS=1 -DRENDERER_ENABLE_SHADOWS=0)
        endif()

        foreach(enable_shadows IN ITEMS 1 0)
            if(enable_shadows)
                set(depth_prepass_suffix "")
            else()
                set(depth_prepass_suffix NoShadows)
            endif()
            if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
                dy_embed_stock_metal_library(StockDepthPrepassFragment${depth_prepass_suffix} kStockDepthPrepassFragment${depth_prepass_suffix} mesh_ps
                    "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}" -DRENDERER_DEPTH_PREPASS=1
                    -DRENDERER_FRAGMENT_ENTRY=depthPrepassFragment)
                dy_embed_stock_metal_library(BindlessDepthPrepassFragment${depth_prepass_suffix} kBindlessDepthPrepassFragment${depth_prepass_suffix} mesh_ps
                    "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}" -DRENDERER_BINDLESS=1 -DRENDERER_DEPTH_PREPASS=1
                    -DRENDERER_FRAGMENT_ENTRY=depthPrepassFragment)
            else()
                dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag StockDepthPrepassFragment${depth_prepass_suffix} kStockDepthPrepassFragment${depth_prepass_suffix}
                    "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}" -DRENDERER_DEPTH_PREPASS=1)
                dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag BindlessDepthPrepassFragment${depth_prepass_suffix} kBindlessDepthPrepassFragment${depth_prepass_suffix}
                    "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}" -DRENDERER_BINDLESS=1 -DRENDERER_DEPTH_PREPASS=1)
            endif()
        endforeach()

        foreach(no_area_depth IN ITEMS 0 1)
            foreach(no_area_shadows IN ITEMS 1 0)
                foreach(no_area_bindless IN ITEMS 0 1)
                    if(no_area_bindless)
                        set(no_area_name Bindless)
                    else()
                        set(no_area_name Stock)
                    endif()
                    if(no_area_depth)
                        string(APPEND no_area_name NoAreaDepthPrepassFragment)
                    else()
                        string(APPEND no_area_name NoAreaFragment)
                    endif()
                    if(NOT no_area_shadows)
                        string(APPEND no_area_name NoShadows)
                    endif()
                    set(no_area_defines -DRENDERER_ENABLE_AREA_LIGHTS=0
                        "-DRENDERER_ENABLE_SHADOWS=${no_area_shadows}"
                        "-DRENDERER_BINDLESS=${no_area_bindless}"
                        "-DRENDERER_DEPTH_PREPASS=${no_area_depth}")
                    if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
                        dy_embed_stock_metal_library(${no_area_name} k${no_area_name} mesh_ps
                            ${no_area_defines} -DRENDERER_FRAGMENT_ENTRY=noAreaFragment)
                    else()
                        dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag
                            ${no_area_name} k${no_area_name} ${no_area_defines})
                    endif()
                endforeach()
            endforeach()
        endforeach()

        if(DY_BACKEND_NORMALIZED STREQUAL "VULKAN")
            dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.glsl" frag
                StockNoAreaDepthPrepassFragmentGatherPcf kStockNoAreaDepthPrepassFragmentGatherPcf
                -DRENDERER_ENABLE_AREA_LIGHTS=0 -DRENDERER_ENABLE_SHADOWS=1 -DRENDERER_BINDLESS=0
                -DRENDERER_DEPTH_PREPASS=1 -DRENDERER_PCF_GATHER=1)
            dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.glsl" frag
                BindlessNoAreaDepthPrepassFragmentGatherPcf kBindlessNoAreaDepthPrepassFragmentGatherPcf
                -DRENDERER_ENABLE_AREA_LIGHTS=0 -DRENDERER_ENABLE_SHADOWS=1 -DRENDERER_BINDLESS=1
                -DRENDERER_DEPTH_PREPASS=1 -DRENDERER_PCF_GATHER=1)
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
    endif()
endmacro()
