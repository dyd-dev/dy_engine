# ===== ===== GLFW ===== =====
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

# ===== ===== Fetch ===== =====
message(STATUS "Download and Configure glfw...")

include(FetchContent)
FetchContent_Declare(
	glfw
	GIT_REPOSITORY "https://github.com/glfw/glfw.git"
	GIT_TAG "3.4"
)
FetchContent_MakeAvailable(glfw)

# stb_image 자동 다운로드 설정
FetchContent_Declare(
    stb
    GIT_REPOSITORY "https://github.com/nothings/stb.git"
    GIT_TAG "master"
)
FetchContent_MakeAvailable(stb)

target_link_libraries(${PROJECT_NAME} PRIVATE glfw)
target_compile_definitions(${PROJECT_NAME} PRIVATE GLFW_INCLUDE_NONE)

target_include_directories(${PROJECT_NAME} PUBLIC ${stb_SOURCE_DIR})

# fastgltf 자동 다운로드 설정
FetchContent_Declare(
    fastgltf
    GIT_REPOSITORY "https://github.com/spnda/fastgltf.git"
    GIT_TAG "v0.9.0" # 최신 버전
)
FetchContent_MakeAvailable(fastgltf)
target_link_libraries(${PROJECT_NAME} PUBLIC fastgltf::fastgltf)

# ufbx 자동 다운로드 설정
FetchContent_Declare(
    ufbx
    GIT_REPOSITORY "https://github.com/ufbx/ufbx.git"
    GIT_TAG "master"
)
FetchContent_MakeAvailable(ufbx)
target_include_directories(${PROJECT_NAME} PUBLIC ${ufbx_SOURCE_DIR})
set_source_files_properties("${ufbx_SOURCE_DIR}/ufbx.c" PROPERTIES LANGUAGE CXX)
target_sources(${PROJECT_NAME} PRIVATE "${ufbx_SOURCE_DIR}/ufbx.c")
