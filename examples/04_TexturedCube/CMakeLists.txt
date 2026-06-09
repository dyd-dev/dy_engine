set(EXAMPLE_NAME "TexturedCube")

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

dy_setup_example(${EXAMPLE_NAME})

add_custom_command(TARGET ${EXAMPLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Textures"
        "$<TARGET_FILE_DIR:${EXAMPLE_NAME}>/Textures"
    VERBATIM)
