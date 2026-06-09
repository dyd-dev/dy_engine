set(EXAMPLE_NAME "HelloRenderer")

add_executable(${EXAMPLE_NAME}
    main.cpp
    Shaders/mesh_vs.glsl
    Shaders/mesh_ps.glsl
    Shaders/mesh_shadow_vs.glsl
    Shaders/mesh_vs.hlsl
    Shaders/mesh_ps.hlsl
)

set_source_files_properties(
    Shaders/mesh_vs.glsl
    Shaders/mesh_ps.glsl
    Shaders/mesh_shadow_vs.glsl
    Shaders/mesh_vs.hlsl
    Shaders/mesh_ps.hlsl
    PROPERTIES HEADER_FILE_ONLY TRUE
)

target_link_libraries(${EXAMPLE_NAME} PRIVATE ${PROJECT_NAME})
target_include_directories(${EXAMPLE_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(TARGET Backend_D3D12)
    target_link_libraries(${EXAMPLE_NAME} PRIVATE Backend_D3D12)
elseif(TARGET Backend_Metal)
    target_link_libraries(${EXAMPLE_NAME} PRIVATE Backend_Metal)
elseif(TARGET Backend_Vulkan)
    target_link_libraries(${EXAMPLE_NAME} PRIVATE Backend_Vulkan)
elseif(TARGET Backend_Null)
    target_link_libraries(${EXAMPLE_NAME} PRIVATE Backend_Null)
endif()

target_compile_definitions(${EXAMPLE_NAME} PRIVATE DY_SHADER_DIR="./Shaders")

set_property(TARGET ${EXAMPLE_NAME} PROPERTY FOLDER ${EXAMPLE_NAME})
set_property(TARGET ${EXAMPLE_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:${EXAMPLE_NAME}>")

# 라이브러리 복사 + 셰이더 컴파일(빌드 시). 렌더러는 컴파일된 바이너리(.spv/.dxil)를 읽는다.
# 통합 헤더(.inc)는 src 아래 있으므로 -I 로 인클루드 경로를 준다.
dy_example_shaders(${EXAMPLE_NAME})
