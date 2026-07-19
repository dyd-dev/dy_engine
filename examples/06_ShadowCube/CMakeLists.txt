set(EXAMPLE_NAME "ShadowCube")

add_executable(${EXAMPLE_NAME}
    main.cpp
    Shaders/mesh_vs.glsl
    Shaders/mesh_ps.glsl
    Shaders/mesh_shadow_vs.glsl
    Shaders/mesh_vs.hlsl
    Shaders/mesh_ps.hlsl
    Shaders/mesh_shadow_vs.hlsl
    Shaders/mesh_vs.metal
    Shaders/mesh_ps.metal
    Shaders/mesh_shadow_vs.metal
)

set_source_files_properties(
    Shaders/mesh_vs.glsl
    Shaders/mesh_ps.glsl
    Shaders/mesh_shadow_vs.glsl
    Shaders/mesh_vs.hlsl
    Shaders/mesh_ps.hlsl
    Shaders/mesh_shadow_vs.hlsl
    Shaders/mesh_vs.metal
    Shaders/mesh_ps.metal
    Shaders/mesh_shadow_vs.metal
    PROPERTIES HEADER_FILE_ONLY TRUE
)

dy_setup_example(${EXAMPLE_NAME})
