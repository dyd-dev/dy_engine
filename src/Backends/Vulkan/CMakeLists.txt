add_library(dy_backend_vulkan OBJECT
	VulkanFactory.cpp
	VulkanDevice.cpp
	VulkanCommandList.cpp
	VulkanResources.cpp
	VulkanSwapchain.cpp
	VulkanPipeline.cpp
)

if(ANDROID)
	if(NOT TARGET Vulkan::Vulkan)
		find_library(VULKAN_LIB vulkan)
		add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
		set_target_properties(Vulkan::Vulkan PROPERTIES
			IMPORTED_LOCATION "${VULKAN_LIB}"
		)
	endif()
else()
	find_package(Vulkan REQUIRED)
endif()

target_link_libraries(dy_backend_vulkan PRIVATE
	dy_engine_options
	Vulkan::Vulkan
)

if(NOT ANDROID)
	target_link_libraries(dy_backend_vulkan PRIVATE glfw)
	target_compile_definitions(dy_backend_vulkan PRIVATE GLFW_INCLUDE_NONE)
else()
	target_link_libraries(dy_backend_vulkan PRIVATE android log)
endif()

# The Vulkan Backend is an object library, so the final engine target still links
# Vulkan privately without exporting Vulkan headers to users.
target_link_libraries(${PROJECT_NAME} PRIVATE Vulkan::Vulkan)

target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:dy_backend_vulkan>)
