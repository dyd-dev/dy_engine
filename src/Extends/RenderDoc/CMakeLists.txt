add_library(dy_renderdoc STATIC EXCLUDE_FROM_ALL RenderDoc.cpp)
add_library(dyf::RenderDoc ALIAS dy_renderdoc)

target_link_libraries(dy_renderdoc PUBLIC dy_engine_options)
if(DY_ENABLE_RENDERDOC AND CMAKE_DL_LIBS)
    target_link_libraries(dy_renderdoc PRIVATE ${CMAKE_DL_LIBS})
endif()

if(DY_ENABLE_RENDERDOC)
    target_include_directories(dy_renderdoc PRIVATE "${DY_RENDERDOC_INCLUDE_DIR}")
endif()
