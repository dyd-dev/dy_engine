add_library(Backend_Vulkan OBJECT
	VulkanDevice.cpp
	VulkanCommandList.cpp
	VulkanResources.cpp
	VulkanSwapchain.cpp
	VulkanPipeline.cpp
	VulkanRuntimePolicy.cpp
)

find_package(Vulkan REQUIRED)

target_link_libraries(Backend_Vulkan PRIVATE
	Engine_Options
	Vulkan::Vulkan
	glfw
)
target_compile_definitions(Backend_Vulkan PRIVATE GLFW_INCLUDE_NONE)

# Backend_Vulkan is an object library, so the final engine target still links
# Vulkan privately without exporting Vulkan headers to users.
target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)

target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:Backend_Vulkan>)
