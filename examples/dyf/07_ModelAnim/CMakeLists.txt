cmake_minimum_required(VERSION 3.20)
project(DyfModelAnim LANGUAGES CXX)

get_filename_component(engine_default "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(DY_ENGINE_SOURCE_DIR "${engine_default}" CACHE PATH "dy_engine source directory.")
if(NOT TARGET dy_engine)
    option(DY_EXTEND_MODEL "Build the optional Model extension." ON)
    add_subdirectory("${DY_ENGINE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dy_engine" EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET dyf::Model)
    message(FATAL_ERROR "07_ModelAnim requires DY_EXTEND_MODEL=ON.")
endif()

add_executable(DyfModelAnim main.cpp)
target_link_libraries(DyfModelAnim PRIVATE dyf::Model)

# main.cpp의 상대 경로를 실행 파일의 작업 폴더에서 사용할 수 있도록 복사한다.
add_custom_command(TARGET DyfModelAnim POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:DyfModelAnim>/Models"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Models/Fox.glb" "$<TARGET_FILE_DIR:DyfModelAnim>/Models/Fox.glb"
    VERBATIM)
set_target_properties(DyfModelAnim PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:DyfModelAnim>")
