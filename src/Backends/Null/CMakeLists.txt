# ===== ===== Backend Target ===== =====
set(BACKEND_NAME "Backend_Null")

add_library(${BACKEND_NAME} STATIC
	NullDevice.cpp
)
target_link_libraries(${BACKEND_NAME} PRIVATE
	Engine_Options
)

message(STATUS "[${PROJECT_NAME}] Ready to Use ${BACKEND_NAME}")