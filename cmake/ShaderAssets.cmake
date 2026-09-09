if(USE_D3D12)
    set(DY_BACKEND_NORMALIZED D3D12)
elseif(USE_METAL)
    set(DY_BACKEND_NORMALIZED METAL)
elseif(USE_VULKAN)
    set(DY_BACKEND_NORMALIZED VULKAN)
else()
    set(DY_BACKEND_NORMALIZED NULL)
endif()

set(DY_SHADER_PUBLIC_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/src/Public")
set(DY_STOCK_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/Graphics/Private/Shaders")
set(DY_STOCK_SHADER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/stock_shaders")
set(DY_STOCK_SHADER_EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBinary.cmake")

# Every stock program must ship all three native representations, even when the
# current host can only run one of the platform compilers.
set(DY_STOCK_SHADER_NATIVE_SOURCES)
foreach(language IN ITEMS glsl hlsl metal)
    set(programs mesh_vs mesh_ps mesh_shadow_vs)
    if(language STREQUAL "metal")
        list(APPEND programs canvas)
    else()
        list(APPEND programs canvas_vs canvas_ps)
    endif()
    foreach(program IN LISTS programs)
        set(source "${DY_STOCK_SHADER_SOURCE_DIR}/${program}.${language}")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "Missing ${language} stock shader: ${source}")
        endif()
        list(APPEND DY_STOCK_SHADER_NATIVE_SOURCES "${source}")
    endforeach()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DY_STOCK_SHADER_NATIVE_SOURCES})

# Null has no native shader bytecode. Do not silently select GLSL for this backend.
if(DY_BACKEND_NORMALIZED STREQUAL "NULL")
    target_compile_definitions(dy_graphics_assets PRIVATE DY_NO_NATIVE_SHADERS=1)
    return()
endif()

set(DY_STOCK_SHADER_LAYOUT "${DY_SHADER_PUBLIC_INCLUDE_DIR}/Graphics/ShaderInterop/StockShaderLayout.inc")
file(GLOB DY_STOCK_SHADER_HELPERS CONFIGURE_DEPENDS "${DY_STOCK_SHADER_SOURCE_DIR}/*.inc" "${DY_STOCK_SHADER_SOURCE_DIR}/Skinning.*")
list(APPEND DY_STOCK_SHADER_HELPERS "${DY_SHADER_PUBLIC_INCLUDE_DIR}/Graphics/ShaderInterop/LightingTypes.inc")
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
        else()
            set(profile ps_6_0)
        endif()
        add_custom_command(
            OUTPUT "${binary}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
            COMMAND "${DY_DXC}" -T "${profile}" -E main ${shader_defines} -I "${DY_STOCK_SHADER_SOURCE_DIR}" -I "${DY_SHADER_PUBLIC_INCLUDE_DIR}" -Fo "${binary}" "${source}"
            DEPENDS "${source}" "${DY_STOCK_SHADER_LAYOUT}" ${DY_STOCK_SHADER_HELPERS}
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
            COMMAND "${DY_GLSLC}" "-fshader-stage=${stage}" ${shader_defines} -I "${DY_STOCK_SHADER_SOURCE_DIR}" -I "${DY_SHADER_PUBLIC_INCLUDE_DIR}" "${source}" -o "${binary}"
            DEPENDS "${source}" "${DY_STOCK_SHADER_LAYOUT}" ${DY_STOCK_SHADER_HELPERS}
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

if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    find_program(DY_XCRUN NAMES xcrun)
    if(NOT DY_XCRUN)
        message(FATAL_ERROR "xcrun is required to build the Metal stock shaders")
    endif()

    set(metal_air_files)
    set(source "${DY_STOCK_SHADER_SOURCE_DIR}/mesh_shadow_vs.metal")
    set(air "${DY_STOCK_SHADER_GENERATED_DIR}/mesh_shadow_vs.air")
    add_custom_command(
        OUTPUT "${air}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
        COMMAND "${DY_XCRUN}" -sdk macosx metal -I "${DY_STOCK_SHADER_SOURCE_DIR}" -I "${DY_SHADER_PUBLIC_INCLUDE_DIR}" -c "${source}" -o "${air}"
        DEPENDS "${source}" "${DY_STOCK_SHADER_LAYOUT}" ${DY_STOCK_SHADER_HELPERS}
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
            COMMAND "${DY_XCRUN}" -sdk macosx metal -I "${DY_STOCK_SHADER_SOURCE_DIR}" -I "${DY_SHADER_PUBLIC_INCLUDE_DIR}"
                "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}"
                "-DRENDERER_VERTEX_ENTRY=${vertex_entry}"
                -c "${source}" -o "${air}"
            DEPENDS "${source}" "${DY_STOCK_SHADER_LAYOUT}" ${DY_STOCK_SHADER_HELPERS}
            VERBATIM)
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
            COMMAND "${DY_XCRUN}" -sdk macosx metal -I "${DY_STOCK_SHADER_SOURCE_DIR}" -I "${DY_SHADER_PUBLIC_INCLUDE_DIR}"
                "-DRENDERER_ENABLE_SHADOWS=${enable_shadows}"
                "-DRENDERER_FRAGMENT_ENTRY=${fragment_entry}"
                -c "${source}" -o "${air}"
            DEPENDS "${source}" "${DY_STOCK_SHADER_LAYOUT}" ${DY_STOCK_SHADER_HELPERS}
            VERBATIM)
        list(APPEND metal_air_files "${air}")
    endforeach()

    set(metal_library "${DY_STOCK_SHADER_GENERATED_DIR}/StockShaders.metallib")
    add_custom_command(
        OUTPUT "${metal_library}"
        COMMAND "${DY_XCRUN}" -sdk macosx metallib ${metal_air_files} -o "${metal_library}"
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
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_vs.${shader_extension}" vert StockVertexShader kStockVertexShader -DRENDERER_ENABLE_SHADOWS=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_vs.${shader_extension}" vert StockVertexShaderNoShadows kStockVertexShaderNoShadows -DRENDERER_ENABLE_SHADOWS=0)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag StockFragmentShader kStockFragmentShader -DRENDERER_ENABLE_SHADOWS=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_ps.${shader_extension}" frag StockFragmentShaderNoShadows kStockFragmentShaderNoShadows -DRENDERER_ENABLE_SHADOWS=0)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/mesh_shadow_vs.${shader_extension}" vert StockShadowVertexShader kStockShadowVertexShader)
endif()

if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    set(canvas_air "${DY_STOCK_SHADER_GENERATED_DIR}/Canvas.air")
    set(canvas_library "${DY_STOCK_SHADER_GENERATED_DIR}/Canvas.metallib")
    set(canvas_header "${DY_STOCK_SHADER_GENERATED_DIR}/CanvasLibrary.h")
    add_custom_command(OUTPUT "${canvas_header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DY_STOCK_SHADER_GENERATED_DIR}"
        COMMAND "${DY_XCRUN}" -sdk macosx metal -c "${DY_STOCK_SHADER_SOURCE_DIR}/canvas.metal" -o "${canvas_air}"
        COMMAND "${DY_XCRUN}" -sdk macosx metallib "${canvas_air}" -o "${canvas_library}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${canvas_library}" "-DOUTPUT=${canvas_header}" -DSYMBOL=kCanvasLibrary -P "${DY_STOCK_SHADER_EMBED_SCRIPT}"
        DEPENDS "${DY_STOCK_SHADER_SOURCE_DIR}/canvas.metal" "${DY_STOCK_SHADER_EMBED_SCRIPT}" VERBATIM)
    list(APPEND DY_STOCK_SHADER_HEADERS "${canvas_header}")
    set(canvas_bundle "#include \"CanvasLibrary.h\"\nnamespace dy::Graphics::Private::generated { inline const StockShaderAssets canvas = {{kCanvasLibrary,kCanvasLibrarySize,\"canvasVertex\"},{kCanvasLibrary,kCanvasLibrarySize,\"canvasFragment\"},{}}; }\n")
else()
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/canvas_vs.${shader_extension}" vert CanvasVertex kCanvasVertex)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/canvas_ps.${shader_extension}" frag CanvasFragment kCanvasFragment)
    set(canvas_bundle "#include \"CanvasVertex.h\"\n#include \"CanvasFragment.h\"\nnamespace dy::Graphics::Private::generated { inline const StockShaderAssets canvas = {{kCanvasVertex,kCanvasVertexSize,\"main\"},{kCanvasFragment,kCanvasFragmentSize,\"main\"},{}}; }\n")
endif()
file(GENERATE OUTPUT "${DY_STOCK_SHADER_GENERATED_DIR}/CanvasShaderBundle.h" CONTENT "${canvas_bundle}")

add_custom_target(dy_stock_shaders DEPENDS ${DY_STOCK_SHADER_HEADERS})
add_dependencies(dy_graphics_assets dy_stock_shaders)
target_include_directories(dy_graphics_assets PRIVATE "${DY_STOCK_SHADER_GENERATED_DIR}")

if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    set(bundle "#include \"StockMetalLibrary.h\"\nnamespace dy::Graphics::Private::generated {\ninline const StockShaderAssets withShadows = {{kStockMetalLibrary,kStockMetalLibrarySize,\"vertexShader\"},{kStockMetalLibrary,kStockMetalLibrarySize,\"fragmentShader\"},{kStockMetalLibrary,kStockMetalLibrarySize,\"shadowVertexShader\"}};\ninline const StockShaderAssets withoutShadows = {{kStockMetalLibrary,kStockMetalLibrarySize,\"vertexShaderNoShadows\"},{kStockMetalLibrary,kStockMetalLibrarySize,\"fragmentShaderNoShadows\"},{kStockMetalLibrary,kStockMetalLibrarySize,\"shadowVertexShader\"}};\n}\n")
else()
    set(bundle "#include \"StockVertexShader.h\"\n#include \"StockVertexShaderNoShadows.h\"\n#include \"StockFragmentShader.h\"\n#include \"StockFragmentShaderNoShadows.h\"\n#include \"StockShadowVertexShader.h\"\nnamespace dy::Graphics::Private::generated {\ninline const StockShaderAssets withShadows = {{kStockVertexShader,kStockVertexShaderSize,\"main\"},{kStockFragmentShader,kStockFragmentShaderSize,\"main\"},{kStockShadowVertexShader,kStockShadowVertexShaderSize,\"main\"}};\ninline const StockShaderAssets withoutShadows = {{kStockVertexShaderNoShadows,kStockVertexShaderNoShadowsSize,\"main\"},{kStockFragmentShaderNoShadows,kStockFragmentShaderNoShadowsSize,\"main\"},{kStockShadowVertexShader,kStockShadowVertexShaderSize,\"main\"}};\n}\n")
endif()
file(GENERATE OUTPUT "${DY_STOCK_SHADER_GENERATED_DIR}/StockShaderBundle.h" CONTENT "${bundle}")
