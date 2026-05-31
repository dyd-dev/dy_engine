set(EXAMPLE_NAME "Vulkan_Test")

set(VULKAN_TEST_SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Shaders)
set(VULKAN_TEST_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/Shaders)

find_program(VULKAN_TEST_GLSLANG_VALIDATOR
	NAMES glslangValidator
	HINTS ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
	PATH_SUFFIXES Bin bin
)

if(NOT VULKAN_TEST_GLSLANG_VALIDATOR)
	message(FATAL_ERROR "Vulkan_Test requires glslangValidator. Install Vulkan SDK or expose glslangValidator to CMake.")
endif()

set(VULKAN_TEST_SHADER_OUTPUTS
	${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.vert.spv
	${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.frag.spv
)

add_custom_command(
	OUTPUT ${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.vert.spv
	COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_TEST_SHADER_OUTPUT_DIR}
	COMMAND ${VULKAN_TEST_GLSLANG_VALIDATOR} -V ${VULKAN_TEST_SHADER_SOURCE_DIR}/triangle.vert -o ${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.vert.spv
	DEPENDS ${VULKAN_TEST_SHADER_SOURCE_DIR}/triangle.vert
	COMMENT "Compiling Vulkan_Test vertex shader"
	VERBATIM
)

add_custom_command(
	OUTPUT ${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.frag.spv
	COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_TEST_SHADER_OUTPUT_DIR}
	COMMAND ${VULKAN_TEST_GLSLANG_VALIDATOR} -V ${VULKAN_TEST_SHADER_SOURCE_DIR}/triangle.frag -o ${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.frag.spv
	DEPENDS ${VULKAN_TEST_SHADER_SOURCE_DIR}/triangle.frag
	COMMENT "Compiling Vulkan_Test fragment shader"
	VERBATIM
)

add_custom_target(Vulkan_Test_Shaders DEPENDS ${VULKAN_TEST_SHADER_OUTPUTS})

file(TO_CMAKE_PATH "${VULKAN_TEST_SHADER_OUTPUT_DIR}" VULKAN_TEST_SHADER_OUTPUT_DIR_CMAKE)

add_executable(${EXAMPLE_NAME}
	main.cpp
)

add_dependencies(${EXAMPLE_NAME} Vulkan_Test_Shaders)

target_link_libraries(${EXAMPLE_NAME} PRIVATE
	${PROJECT_NAME}
	Backend_Vulkan
)

target_compile_definitions(${EXAMPLE_NAME} PRIVATE
	DY_VULKAN_EXAMPLE_SHADER_DIR="${VULKAN_TEST_SHADER_OUTPUT_DIR_CMAKE}"
)

set_property(TARGET ${EXAMPLE_NAME} PROPERTY FOLDER ${EXAMPLE_NAME})
