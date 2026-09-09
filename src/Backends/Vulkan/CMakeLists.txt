add_library(dy_backend_vulkan OBJECT
	VulkanDevice.cpp
	VulkanCommandList.cpp
	VulkanResources.cpp
	VulkanSwapchain.cpp
	VulkanPipeline.cpp
)

find_package(Vulkan REQUIRED)

target_link_libraries(dy_backend_vulkan PRIVATE
	dy_engine_options
	Vulkan::Vulkan
	glfw
)
target_compile_definitions(dy_backend_vulkan PRIVATE GLFW_INCLUDE_NONE)

# The Vulkan Backend is an object library, so the final engine target still links
# Vulkan privately without exporting Vulkan headers to users.
target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)

target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:dy_backend_vulkan>)
