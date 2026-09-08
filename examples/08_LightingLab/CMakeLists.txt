set(EXAMPLE_NAME "LightingLab")

add_executable(${EXAMPLE_NAME}
    main.cpp
    Shaders/mesh_vs.glsl
    Shaders/mesh_ps.glsl
    Shaders/mesh_shadow_vs.glsl
    Shaders/tone_map_vs.glsl
    Shaders/tone_map_ps.glsl
)

set_source_files_properties(
    Shaders/mesh_vs.glsl
    Shaders/mesh_ps.glsl
    Shaders/mesh_shadow_vs.glsl
    Shaders/tone_map_vs.glsl
    Shaders/tone_map_ps.glsl
    PROPERTIES HEADER_FILE_ONLY TRUE
)

dy_setup_example(${EXAMPLE_NAME})
