set(BACKEND_NAME "Backend_D3D12")

add_library(${BACKEND_NAME} STATIC
	D3D12Device.cpp
)
target_link_libraries(${BACKEND_NAME} PRIVATE
	Engine_Options
)

message(STATUS "[${PROJECT_NAME}] Ready to Use ${BACKEND_NAME} stub")
