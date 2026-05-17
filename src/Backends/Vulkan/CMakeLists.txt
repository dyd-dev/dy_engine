add_library(Backend_Vulkan STATIC
	VulkanDevice.cpp
	VulkanCommandList.cpp
	VulkanResources.cpp
	VulkanSwapchain.cpp
	VulkanPipeline.cpp
)

find_package(Vulkan REQUIRED)

set(VULKAN_SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Shaders)
set(VULKAN_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated/vulkan_shaders)
set(VULKAN_SHADER_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src)
set(VULKAN_SHADER_LAYOUT_FILE ${VULKAN_SHADER_INCLUDE_DIR}/RHI/RendererShaderLayout.inc)

find_program(VULKAN_GLSLANG_VALIDATOR
	NAMES glslangValidator
	HINTS ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
	PATH_SUFFIXES Bin bin
)

if(NOT VULKAN_GLSLANG_VALIDATOR)
	message(FATAL_ERROR "Vulkan backend triangle sample requires glslangValidator. Install Vulkan SDK or expose glslangValidator to CMake.")
endif()

file(TO_CMAKE_PATH "${VULKAN_SHADER_SOURCE_DIR}" VULKAN_SHADER_SOURCE_DIR_CMAKE)
file(TO_CMAKE_PATH "${VULKAN_SHADER_OUTPUT_DIR}" VULKAN_SHADER_OUTPUT_DIR_CMAKE)
file(TO_CMAKE_PATH "${VULKAN_GLSLANG_VALIDATOR}" VULKAN_GLSLANG_VALIDATOR_CMAKE)

set(VULKAN_SHADER_OUTPUTS
	${VULKAN_SHADER_OUTPUT_DIR}/triangle.vert.spv
	${VULKAN_SHADER_OUTPUT_DIR}/triangle.frag.spv
	${VULKAN_SHADER_OUTPUT_DIR}/triangle_shadow.vert.spv
)

add_custom_command(
	OUTPUT ${VULKAN_SHADER_OUTPUT_DIR}/triangle.vert.spv
	COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_SHADER_OUTPUT_DIR}
	COMMAND ${VULKAN_GLSLANG_VALIDATOR} -I${VULKAN_SHADER_INCLUDE_DIR} -V ${VULKAN_SHADER_SOURCE_DIR}/triangle.vert -o ${VULKAN_SHADER_OUTPUT_DIR}/triangle.vert.spv
	DEPENDS ${VULKAN_SHADER_SOURCE_DIR}/triangle.vert ${VULKAN_SHADER_LAYOUT_FILE}
	COMMENT "Compiling Vulkan vertex shader"
	VERBATIM
)

add_custom_command(
	OUTPUT ${VULKAN_SHADER_OUTPUT_DIR}/triangle.frag.spv
	COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_SHADER_OUTPUT_DIR}
	COMMAND ${VULKAN_GLSLANG_VALIDATOR} -I${VULKAN_SHADER_INCLUDE_DIR} -V ${VULKAN_SHADER_SOURCE_DIR}/triangle.frag -o ${VULKAN_SHADER_OUTPUT_DIR}/triangle.frag.spv
	DEPENDS ${VULKAN_SHADER_SOURCE_DIR}/triangle.frag ${VULKAN_SHADER_LAYOUT_FILE}
	COMMENT "Compiling Vulkan fragment shader"
	VERBATIM
)

add_custom_command(
	OUTPUT ${VULKAN_SHADER_OUTPUT_DIR}/triangle_shadow.vert.spv
	COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_SHADER_OUTPUT_DIR}
	COMMAND ${VULKAN_GLSLANG_VALIDATOR} -I${VULKAN_SHADER_INCLUDE_DIR} -V ${VULKAN_SHADER_SOURCE_DIR}/triangle_shadow.vert -o ${VULKAN_SHADER_OUTPUT_DIR}/triangle_shadow.vert.spv
	DEPENDS ${VULKAN_SHADER_SOURCE_DIR}/triangle_shadow.vert ${VULKAN_SHADER_LAYOUT_FILE}
	COMMENT "Compiling Vulkan shadow vertex shader"
	VERBATIM
)

add_custom_target(VulkanShaders ALL DEPENDS ${VULKAN_SHADER_OUTPUTS})
add_dependencies(Backend_Vulkan VulkanShaders)

target_link_libraries(Backend_Vulkan PRIVATE
	glfw
	Vulkan::Vulkan
	Engine_Options
)

target_link_libraries(${PROJECT_NAME} PRIVATE Backend_Vulkan)

target_include_directories(Backend_Vulkan PRIVATE
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(${PROJECT_NAME} PRIVATE
	${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(Backend_Vulkan PRIVATE
	GLFW_INCLUDE_NONE
	VULKAN_DEFAULT_RENDERER_VERTEX_SHADER_PATH="${VULKAN_SHADER_OUTPUT_DIR_CMAKE}/triangle.vert.spv"
	VULKAN_DEFAULT_RENDERER_PIXEL_SHADER_PATH="${VULKAN_SHADER_OUTPUT_DIR_CMAKE}/triangle.frag.spv"
	VULKAN_DEFAULT_RENDERER_SHADOW_VERTEX_SHADER_PATH="${VULKAN_SHADER_OUTPUT_DIR_CMAKE}/triangle_shadow.vert.spv"
	VULKAN_SHADER_SOURCE_DIR="${VULKAN_SHADER_SOURCE_DIR_CMAKE}"
	VULKAN_GLSLANG_VALIDATOR_PATH="${VULKAN_GLSLANG_VALIDATOR_CMAKE}"
)

if(MSVC)
	target_compile_options(Backend_Vulkan PRIVATE /utf-8 /EHsc)
endif()

