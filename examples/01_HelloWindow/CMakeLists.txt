set(EXAMPLE_NAME "HelloWindow")

add_executable(${EXAMPLE_NAME}
	main.cpp
)

target_link_libraries(${EXAMPLE_NAME} PRIVATE ${PROJECT_NAME})

set_property(TARGET ${EXAMPLE_NAME} PROPERTY FOLDER ${EXAMPLE_NAME})

add_custom_command(TARGET ${EXAMPLE_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:${PROJECT_NAME}>
		$<TARGET_FILE_DIR:${EXAMPLE_NAME}>
)