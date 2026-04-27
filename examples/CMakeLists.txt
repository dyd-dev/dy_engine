add_subdirectory(01_HelloWindow)
add_subdirectory(02_HelloRenderer)

if(USE_VULKAN)
    add_subdirectory(vulkan_test)
endif()

if(USE_D3D12)
    add_subdirectory(D3D12_Test)
endif()

