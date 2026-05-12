set(EXAMPLE_NAME "D3D12_Test")

add_executable(${EXAMPLE_NAME}
	main.cpp
)

target_link_libraries(${EXAMPLE_NAME} PRIVATE ${PROJECT_NAME})

set_property(TARGET ${EXAMPLE_NAME} PROPERTY FOLDER ${EXAMPLE_NAME})

