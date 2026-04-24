add_executable(vulkan_test
    main.cpp
)

target_link_libraries(vulkan_test PRIVATE ${PROJECT_NAME} Backend_Vulkan)

target_include_directories(vulkan_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/Backends/Vulkan
)

# Set IDE folder
set_target_properties(vulkan_test PROPERTIES FOLDER "Examples")

# Shader compilation
set(VULKAN_TEST_SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Shaders)
set(VULKAN_TEST_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated/vulkan_test_shaders)

file(TO_CMAKE_PATH "${VULKAN_TEST_SHADER_SOURCE_DIR}" VULKAN_TEST_SHADER_SOURCE_DIR_CMAKE)
file(TO_CMAKE_PATH "${VULKAN_TEST_SHADER_OUTPUT_DIR}" VULKAN_TEST_SHADER_OUTPUT_DIR_CMAKE)

set(VULKAN_TEST_SHADER_OUTPUTS
    ${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.vert.spv
    ${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.frag.spv
)

add_custom_command(
    OUTPUT ${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.vert.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_TEST_SHADER_OUTPUT_DIR}
    COMMAND ${VULKAN_GLSLANG_VALIDATOR} -V ${VULKAN_TEST_SHADER_SOURCE_DIR}/mesh.vert -o ${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.vert.spv
    DEPENDS ${VULKAN_TEST_SHADER_SOURCE_DIR}/mesh.vert
    COMMENT "Compiling Vulkan Test vertex shader"
    VERBATIM
)

add_custom_command(
    OUTPUT ${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.frag.spv
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VULKAN_TEST_SHADER_OUTPUT_DIR}
    COMMAND ${VULKAN_GLSLANG_VALIDATOR} -V ${VULKAN_TEST_SHADER_SOURCE_DIR}/mesh.frag -o ${VULKAN_TEST_SHADER_OUTPUT_DIR}/triangle.frag.spv
    DEPENDS ${VULKAN_TEST_SHADER_SOURCE_DIR}/mesh.frag
    COMMENT "Compiling Vulkan Test fragment shader"
    VERBATIM
)

add_custom_target(VulkanTestShaders ALL DEPENDS ${VULKAN_TEST_SHADER_OUTPUTS})
add_dependencies(vulkan_test VulkanTestShaders)

target_compile_definitions(vulkan_test PRIVATE
    VULKAN_TEST_SHADER_DIR="${VULKAN_TEST_SHADER_OUTPUT_DIR_CMAKE}"
)


