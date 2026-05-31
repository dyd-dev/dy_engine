add_subdirectory(01_HelloWindow)
add_subdirectory(02_HelloRenderer)

if(USE_D3D12)
	add_subdirectory(D3D12_Test)
	add_subdirectory(D3D12_gltf)
	add_subdirectory(D3D12_FBX)
	add_subdirectory(D3D12_Lighting)
endif()

if(USE_VULKAN)
	add_subdirectory(vulkan_test)
endif()
