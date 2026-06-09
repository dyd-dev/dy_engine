set(EXAMPLE_NAME "07_RenderPath")

add_executable(${EXAMPLE_NAME}
    main.cpp
    Shaders/mesh_vs.glsl
    Shaders/mesh_ps.glsl
    Shaders/mesh_vs.hlsl
    Shaders/mesh_ps.hlsl
)

set_source_files_properties(
    Shaders/mesh_vs.glsl
    Shaders/mesh_ps.glsl
    Shaders/mesh_vs.hlsl
    Shaders/mesh_ps.hlsl
    PROPERTIES HEADER_FILE_ONLY TRUE
)

dy_setup_example(${EXAMPLE_NAME})

# 벤치마크 씬은 05_LoadModel 의 모델 에셋을 재사용한다(작업 폴더로 복사).
add_custom_command(TARGET ${EXAMPLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/examples/05_LoadModel/Models"
        "$<TARGET_FILE_DIR:${EXAMPLE_NAME}>/Models"
    VERBATIM)
