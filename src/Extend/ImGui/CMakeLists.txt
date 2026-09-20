FetchContent_Declare(imgui
    GIT_REPOSITORY "https://github.com/ocornut/imgui.git"
    GIT_TAG "3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c" # v1.92.5-docking
)
FetchContent_MakeAvailable(imgui)

add_library(dy_imgui STATIC
    ImGui.cpp
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
)
add_library(dyf::ImGui ALIAS dy_imgui)
target_link_libraries(dy_imgui PUBLIC dy_engine PRIVATE glfw)
target_include_directories(dy_imgui PUBLIC "${imgui_SOURCE_DIR}")
target_compile_definitions(dy_imgui PRIVATE GLFW_INCLUDE_NONE)

# Keep the Canvas color/vertex ABI and all three native shader sources in one place.
include(CompileShaders)
set(canvas "${CMAKE_CURRENT_SOURCE_DIR}/../../dyf/Shaders")
dy_compile_shader(dy_imgui GuiVertex vert
    GLSL "${canvas}/canvas_vs.glsl" HLSL "${canvas}/canvas_vs.hlsl"
    METAL "${canvas}/canvas.metal" METAL_ENTRY canvasVertex)
dy_compile_shader(dy_imgui GuiFragment frag
    GLSL "${canvas}/canvas_ps.glsl" HLSL "${canvas}/canvas_ps.hlsl"
    METAL "${canvas}/canvas.metal" METAL_ENTRY canvasFragment)
