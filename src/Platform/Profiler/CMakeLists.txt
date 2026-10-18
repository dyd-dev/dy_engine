#include "Connection/Protocol.h"
# Targets.
add_library(dy_profiler STATIC)
add_library(dy_profiler_connection STATIC EXCLUDE_FROM_ALL)
add_library(dyf::Profiler ALIAS dy_profiler)
add_library(dyf::ProfilerConnection ALIAS dy_profiler_connection)

# Sources.
target_sources(dy_profiler PRIVATE Profiler.cpp Collector.cpp)
target_sources(dy_profiler_connection PRIVATE
    Connection/Protocol.cpp
    Connection/Client.cpp
    Connection/Server.cpp)
if(WIN32)
    target_sources(dy_profiler_connection PRIVATE Connection/WindowsPipe.cpp)
endif()

# Library dependencies.
target_link_libraries(dy_profiler PUBLIC dy_engine_options Threads::Threads)
target_link_libraries(dy_profiler_connection PUBLIC dyf::Profiler)
if(WIN32)
    target_link_libraries(dy_profiler_connection PRIVATE advapi32)
endif()

# Include paths.
target_include_directories(dy_profiler PRIVATE "${PROJECT_SOURCE_DIR}/src")
