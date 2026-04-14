set(EXAMPLE_NAME "HelloRenderer")

add_executable(${EXAMPLE_NAME}
	main.cpp
	Shaders/TexturedTriangleVS.hlsl
	Shaders/TexturedTrianglePS.hlsl
	Shaders/TexturedTriangleVS.glsl
	Shaders/TexturedTrianglePS.glsl
	Shaders/TexturedTriangleVS.metal
	Shaders/TexturedTrianglePS.metal
)
set_source_files_properties(
	Shaders/TexturedTriangleVS.hlsl
	Shaders/TexturedTrianglePS.hlsl
	Shaders/TexturedTriangleVS.glsl
	Shaders/TexturedTrianglePS.glsl
	Shaders/TexturedTriangleVS.metal
	Shaders/TexturedTrianglePS.metal
	PROPERTIES HEADER_FILE_ONLY TRUE
)

target_link_libraries(${EXAMPLE_NAME} PRIVATE ${PROJECT_NAME})
target_compile_definitions(${EXAMPLE_NAME} PRIVATE
	DY_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Shaders"
)

set_property(TARGET ${EXAMPLE_NAME} PROPERTY FOLDER ${EXAMPLE_NAME})
set_property(TARGET ${EXAMPLE_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:${EXAMPLE_NAME}>")

add_custom_command(TARGET ${EXAMPLE_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:${PROJECT_NAME}>
		$<TARGET_FILE_DIR:${EXAMPLE_NAME}>
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_CURRENT_SOURCE_DIR}/Shaders"
		"$<TARGET_FILE_DIR:${EXAMPLE_NAME}>/Shaders"
)
