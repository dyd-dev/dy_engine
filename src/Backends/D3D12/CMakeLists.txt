# ===== ===== Requirements ===== =====
# message(STATUS "[${PROJECT_NAME}] Find DirectX 12...")

# ===== ===== Backend Target ===== =====
set(BACKEND_NAME "Backend_D3D12")

# add_library(${BACKEND_NAME} STATIC
# 	D3D12Device.cpp
# )

add_executable(${BACKEND_NAME}
	main.cpp
	D3D12Device.cpp
)

target_link_libraries(${BACKEND_NAME} PRIVATE
	${PROJECT_NAME}
	d3d12 dxgi dxguid d3dcompiler
)

add_custom_command(TARGET ${BACKEND_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
			${CMAKE_CURRENT_SOURCE_DIR}/shaders.hlsl
			$<TARGET_FILE_DIR:${BACKEND_NAME}>/shaders.hlsl
)

target_include_directories(${BACKEND_NAME} PRIVATE ${stb_SOURCE_DIR})
message(STATUS "[${PROJECT_NAME}] Ready to Use ${BACKEND_NAME}")