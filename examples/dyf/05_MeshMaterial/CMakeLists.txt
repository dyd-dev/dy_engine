cmake_minimum_required(VERSION 3.20)
project(DyfMeshMaterial LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()

add_executable(DyfMeshMaterial main.cpp)
target_link_libraries(DyfMeshMaterial PRIVATE dy_engine)

# main.cpp의 상대 경로를 실행 파일의 작업 폴더에서 사용할 수 있도록 복사한다.
add_custom_command(TARGET DyfMeshMaterial POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:DyfMeshMaterial>/Assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Default_albedo.jpg" "$<TARGET_FILE_DIR:DyfMeshMaterial>/Assets/Default_albedo.jpg"
    VERBATIM)
set_target_properties(DyfMeshMaterial PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:DyfMeshMaterial>")
