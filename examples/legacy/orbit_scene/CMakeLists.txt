add_executable(orbit_scene
    main.cpp
)

target_link_libraries(orbit_scene PRIVATE ${PROJECT_NAME})

target_include_directories(orbit_scene PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

set_target_properties(orbit_scene PROPERTIES FOLDER "Examples")
