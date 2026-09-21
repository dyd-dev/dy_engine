# ===== ===== Backend Target ===== =====
set(BACKEND_NAME "dy_backend_null")

add_library(${BACKEND_NAME} OBJECT
	NullDevice.cpp
)
target_include_directories(${BACKEND_NAME} BEFORE PRIVATE "${PROJECT_SOURCE_DIR}/src/Public" "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(${BACKEND_NAME} PRIVATE
	Engine_Options
)

target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:${BACKEND_NAME}>)
message(STATUS "[${PROJECT_NAME}] Ready to Use ${BACKEND_NAME}")
