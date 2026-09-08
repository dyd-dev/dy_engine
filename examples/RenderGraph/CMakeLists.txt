add_executable(RenderGraph main.cpp)
target_link_libraries(RenderGraph PRIVATE ${PROJECT_NAME})
target_include_directories(RenderGraph PRIVATE ${CMAKE_SOURCE_DIR}/src)
