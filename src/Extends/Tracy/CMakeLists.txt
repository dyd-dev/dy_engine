add_library(dy_tracy STATIC EXCLUDE_FROM_ALL Tracy.cpp)
add_library(dyf::Tracy ALIAS dy_tracy)

target_link_libraries(dy_tracy PUBLIC dyf::Profiler)
if(DY_ENABLE_TRACY)
    target_link_libraries(dy_tracy PRIVATE Tracy::TracyClient)
endif()

if(DY_ENABLE_TRACY)
    target_compile_definitions(dy_tracy PRIVATE DY_TRACY_ENABLED=1)
endif()
