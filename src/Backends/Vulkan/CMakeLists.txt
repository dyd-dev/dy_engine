add_library(Backend_Vulkan OBJECT
	VulkanDevice.cpp
	VulkanCommandList.cpp
	VulkanResources.cpp
	VulkanSwapchain.cpp
	VulkanPipeline.cpp
)

find_package(Vulkan REQUIRED)

target_link_libraries(Backend_Vulkan PRIVATE
	Engine_Options
	Vulkan::Vulkan
	glfw
)
target_compile_definitions(Backend_Vulkan PRIVATE GLFW_INCLUDE_NONE)

# IDevice.cpp 팩토리(dy_engine)가 ENABLE_VULKAN 시 VulkanDevice.h -> vulkan.h 를 직접 포함하므로
# dy_engine 에도 Vulkan 인클루드를 전파한다.
target_link_libraries(${PROJECT_NAME} PUBLIC Vulkan::Vulkan)

target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:Backend_Vulkan>)