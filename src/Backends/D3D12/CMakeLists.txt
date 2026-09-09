# ===== ===== Requirements ===== =====
# message(STATUS "[${PROJECT_NAME}] Find DirectX 12...")

# ===== ===== Backend Target ===== =====
set(BACKEND_NAME "dy_backend_d3d12")

add_library(${BACKEND_NAME} OBJECT
	D3D12Device.cpp
	D3D12Buffer.cpp       
	D3D12CommandList.cpp  
	D3D12PipelineState.cpp
	D3D12ResourceSet.cpp
	D3D12Texture.cpp
)

target_link_libraries(${BACKEND_NAME} PRIVATE
	dy_engine_options
)

target_link_libraries(${PROJECT_NAME} PRIVATE d3d12 dxgi dxguid)

target_compile_definitions(${BACKEND_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:${BACKEND_NAME}>)
message(STATUS "[${PROJECT_NAME}] Ready to Use ${BACKEND_NAME}")
