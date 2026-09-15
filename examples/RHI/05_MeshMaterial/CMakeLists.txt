cmake_minimum_required(VERSION 3.20)
project(RhiMeshMaterial LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()
include("${DY_ENGINE_SOURCE_DIR}/cmake/CompileShaders.cmake")

add_executable(RhiMeshMaterial main.cpp)
target_link_libraries(RhiMeshMaterial PRIVATE dy_engine)
set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
dy_compile_shader(RhiMeshMaterial vertex vert
    GLSL "${shader_dir}/mesh_vs.glsl"
    HLSL "${shader_dir}/mesh_vs.hlsl"
    METAL "${shader_dir}/mesh_vs.metal"
    METAL_ENTRY vertexMain
)
dy_compile_shader(RhiMeshMaterial fragment frag
    GLSL "${shader_dir}/mesh_ps.glsl"
    HLSL "${shader_dir}/mesh_ps.hlsl"
    METAL "${shader_dir}/mesh_ps.metal"
    METAL_ENTRY fragmentMain
)

# main.cpp의 상대 경로를 실행 파일의 작업 폴더에서 사용할 수 있도록 복사한다.
add_custom_command(TARGET RhiMeshMaterial POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:RhiMeshMaterial>/Assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Default_albedo.jpg" "$<TARGET_FILE_DIR:RhiMeshMaterial>/Assets/Default_albedo.jpg"
    VERBATIM)
set_target_properties(RhiMeshMaterial PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:RhiMeshMaterial>")
