add_library(Backend_D3D12 STATIC
	D3D12Device.cpp
)

target_link_libraries(Backend_D3D12 PRIVATE
	glfw
	d3d12.lib
	dxgi.lib
	d3dcompiler.lib
)

target_include_directories(Backend_D3D12 PRIVATE
	${CMAKE_SOURCE_DIR}/src
)