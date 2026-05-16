set(BACKEND_NAME "Backend_Metal")

add_library(${BACKEND_NAME} STATIC
	MetalDevice.cpp
)
target_link_libraries(${BACKEND_NAME} PRIVATE
	Engine_Options
)

message(STATUS "[${PROJECT_NAME}] Ready to Use ${BACKEND_NAME} stub")
