# ===== ===== Backend Target ===== =====
set(BACKEND_NAME "Backend_Null")

add_library(${BACKEND_NAME} OBJECT
	NullDevice.cpp
)
target_link_libraries(${BACKEND_NAME} PRIVATE
	Engine_Options
)

target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:${BACKEND_NAME}>)
message(STATUS "[${PROJECT_NAME}] Ready to Use ${BACKEND_NAME}")