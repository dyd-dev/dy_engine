# ===== ===== Requirements ===== =====
# message(STATUS "[${PROJECT_NAME}] Find DirectX 12...")

# ===== ===== Backend Target ===== =====
set(BACKEND_NAME "Backend_D3D12")

add_library(${BACKEND_NAME} STATIC
	D3D12Device.cpp
)
target_link_libraries(${BACKEND_NAME} PRIVATE
	Engine_Options
	d3d12 dxgi dxguid d3dcompiler
)

message(STATUS "[${PROJECT_NAME}] Ready to Use ${BACKEND_NAME}")