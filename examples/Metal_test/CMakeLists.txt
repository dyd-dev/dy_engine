set(EXAMPLE_NAME "Metal_test")

add_executable(${EXAMPLE_NAME}
    main.mm
)

set_source_files_properties(main.mm PROPERTIES
    COMPILE_FLAGS "-x objective-c++"
)

target_link_libraries(${EXAMPLE_NAME} PRIVATE
    ${PROJECT_NAME}
    Backend_Metal
)

set_property(TARGET ${EXAMPLE_NAME} PROPERTY FOLDER ${EXAMPLE_NAME})

# 셰이더 컴파일
set(METAL_SHADER ${CMAKE_CURRENT_SOURCE_DIR}/Shaders.metal)
set(METALLIB_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/default.metallib)

add_custom_command(
    OUTPUT ${METALLIB_OUTPUT}
    COMMAND xcrun -sdk macosx metal -c ${METAL_SHADER} -o ${CMAKE_CURRENT_BINARY_DIR}/Shaders.air
    COMMAND xcrun -sdk macosx metallib ${CMAKE_CURRENT_BINARY_DIR}/Shaders.air -o ${METALLIB_OUTPUT}
    DEPENDS ${METAL_SHADER}
)
target_link_libraries(${EXAMPLE_NAME} PRIVATE
    ${PROJECT_NAME}
    Backend_Metal
    "-framework MetalKit"
)

add_custom_target(Metal_Shaders ALL DEPENDS ${METALLIB_OUTPUT})
add_dependencies(${EXAMPLE_NAME} Metal_Shaders)
