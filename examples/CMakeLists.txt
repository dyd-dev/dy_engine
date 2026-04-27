add_subdirectory(01_HelloWindow)
add_subdirectory(02_HelloRenderer)

if(USE_VULKAN)
    add_subdirectory(vulkan_test)
    add_subdirectory(vulkan_lighting_volume)
endif()
