dy_example_support(BUILD_ONLY metal REASON "Metal shaders are not implemented")
set(EXAMPLE_NAME "LoadModel")

add_executable(${EXAMPLE_NAME}
    main.cpp
    LoadModelOptions.cpp
    LoadModelOptions.h
    LoadModelPlayback.cpp
    LoadModelPlayback.h
    Shaders/mesh_vs.glsl
    Shaders/mesh_ps.glsl
    Shaders/mesh_shadow_vs.glsl
	Shaders/mesh_skinning_cs.glsl
    Shaders/mesh_vs.hlsl
    Shaders/mesh_ps.hlsl
)

set_source_files_properties(
    Shaders/mesh_vs.glsl
    Shaders/mesh_ps.glsl
    Shaders/mesh_shadow_vs.glsl
	Shaders/mesh_skinning_cs.glsl
    Shaders/mesh_vs.hlsl
    Shaders/mesh_ps.hlsl
    PROPERTIES HEADER_FILE_ONLY TRUE
)

dy_setup_example(${EXAMPLE_NAME})
