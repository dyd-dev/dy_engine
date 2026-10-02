add_library(dy_extend_model STATIC
    Animation.cpp
    ModelShaderAssets.cpp
    ModelRenderer.cpp
    Model.cpp
    ModelScene.cpp
    ModelLoader.cpp
    ModelLoaderCommon.cpp
    GltfModelLoader.cpp
    UfbxModelLoader.cpp
)
add_library(dyf::Model ALIAS dy_extend_model)
target_link_libraries(dy_extend_model PUBLIC dy_engine)
target_include_directories(dy_extend_model PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Model을 선택한 경우에만 변형 셰이더를 생성한다.
include("${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ShaderAssets.cmake")

# fastgltf 자동 다운로드 설정
FetchContent_Declare(
    fastgltf
    GIT_REPOSITORY "https://github.com/spnda/fastgltf.git"
    GIT_TAG "v0.9.0" # 최신 버전
)
# fastgltf v0.9.0 is validated with its pinned simdjson 3.12.3. Do not let a
# host package manager silently substitute a newer, incompatible header.
set(CMAKE_DISABLE_FIND_PACKAGE_simdjson ON)
FetchContent_MakeAvailable(fastgltf)
unset(CMAKE_DISABLE_FIND_PACKAGE_simdjson)
target_link_libraries(dy_extend_model PRIVATE fastgltf::fastgltf)

# ufbx 자동 다운로드 설정
FetchContent_Declare(
    ufbx
    GIT_REPOSITORY "https://github.com/ufbx/ufbx.git"
    GIT_TAG "fcc5d6ba444cfd3eb80677dba5e37e493941abe5"
)
FetchContent_MakeAvailable(ufbx)
target_include_directories(dy_extend_model PRIVATE ${ufbx_SOURCE_DIR})
set_source_files_properties("${ufbx_SOURCE_DIR}/ufbx.c" PROPERTIES LANGUAGE CXX)
target_sources(dy_extend_model PRIVATE "${ufbx_SOURCE_DIR}/ufbx.c")
