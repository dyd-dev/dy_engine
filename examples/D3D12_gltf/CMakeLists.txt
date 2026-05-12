set(EXAMPLE_NAME "D3D12_gltf")

add_executable(${EXAMPLE_NAME}
	main.cpp
)

target_link_libraries(${EXAMPLE_NAME} PRIVATE ${PROJECT_NAME})

set_property(TARGET ${EXAMPLE_NAME} PROPERTY FOLDER ${EXAMPLE_NAME})

# Copy assets and shaders to build directory
add_custom_command(TARGET ${EXAMPLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/Models"
    "$<TARGET_FILE_DIR:${EXAMPLE_NAME}>/Models"
)

add_custom_command(TARGET ${EXAMPLE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/Shaders"
    "$<TARGET_FILE_DIR:${EXAMPLE_NAME}>/Shaders"
)