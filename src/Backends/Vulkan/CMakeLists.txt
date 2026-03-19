# ===== ===== Requirements ===== =====
message(STATUS "[${PROJECT_NAME}] Find Vulkan Package...")

find_package(Vulkan REQUIRED)

# ===== ===== Backend Target ===== =====
set(BACKEND_NAME "Backend_Vulkan")

add_library(${BACKEND_NAME} STATIC
	VulkanDevice.cpp
)
target_link_libraries(${BACKEND_NAME} PRIVATE
	Engine_Options
	Vulkan::Vulkan
)

message(STATUS "[${PROJECT_NAME}] Ready to Use ${BACKEND_NAME}")