add_executable(dy_compare_images CompareImages.cpp)
target_compile_features(dy_compare_images PRIVATE cxx_std_17)
target_include_directories(dy_compare_images PRIVATE "${stb_SOURCE_DIR}")

add_executable(dy_asset_tests AssetTests.cpp)
target_link_libraries(dy_asset_tests PRIVATE dy_graphics_assets)
if(NOT DY_BACKEND_NORMALIZED STREQUAL "NULL")
    target_compile_definitions(dy_asset_tests PRIVATE DY_TEST_NATIVE_SHADERS=1)
endif()

# Explicit opt-in: this test creates a native window. CPU/Null tests remain
# usable without a display, and examples contain no capture/timeout runner.
if(NOT DY_BACKEND_NORMALIZED STREQUAL "NULL")
    add_executable(dy_native_rendering_tests NativeRenderingTests.cpp)
    target_link_libraries(dy_native_rendering_tests PRIVATE dy_engine)
    option(DY_TEST_NATIVE_RENDERING "Run windowed Graphics/RHI pixel and skinning tests." OFF)
    if(DY_TEST_NATIVE_RENDERING)
        add_test(NAME graphics.native_composition COMMAND dy_native_rendering_tests
            "${PROJECT_SOURCE_DIR}/examples/Graphics/07_ModelAnim/Models/Fox.glb"
            "${CMAKE_CURRENT_BINARY_DIR}/native-images")
        set_tests_properties(graphics.native_composition PROPERTIES TIMEOUT 45)
    endif()
endif()
add_test(NAME assets.core COMMAND dy_asset_tests "${CMAKE_CURRENT_BINARY_DIR}/image-output.ppm")

set(DY_TEST_FONT "" CACHE FILEPATH "TrueType font for CPU font and atlas tests.")
if(DY_TEST_FONT)
    add_test(NAME assets.font COMMAND dy_asset_tests "${CMAKE_CURRENT_BINARY_DIR}/font-output.ppm" "${DY_TEST_FONT}")
endif()

add_executable(dy_resource_scope_tests ResourceScopeTests.cpp)
target_compile_features(dy_resource_scope_tests PRIVATE cxx_std_17)
target_include_directories(dy_resource_scope_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/Public")
add_test(NAME rhi.resource_scope COMMAND dy_resource_scope_tests)

add_executable(dy_model_asset_tests ModelAssetTests.cpp)
target_link_libraries(dy_model_asset_tests PRIVATE dy_engine)
foreach(layer IN ITEMS Graphics RHI)
    add_test(NAME assets.models.${layer} COMMAND dy_model_asset_tests
        "${PROJECT_SOURCE_DIR}/examples/${layer}/07_ModelAnim/Models/Fox.glb"
        "${PROJECT_SOURCE_DIR}/examples/${layer}/07_ModelAnim/Models/shiba/scene.FBX"
        "${PROJECT_SOURCE_DIR}/examples/${layer}/02_Image/Assets/Default_albedo.jpg")
endforeach()
if(DY_BACKEND_NORMALIZED STREQUAL "NULL")
    add_executable(dy_renderer_composition_tests RendererCompositionTests.cpp)
    target_link_libraries(dy_renderer_composition_tests PRIVATE dy_engine)
    set(composition_args "${PROJECT_SOURCE_DIR}/examples/Graphics/07_ModelAnim/Models/Fox.glb")
    if(DY_TEST_FONT)
        list(APPEND composition_args "${DY_TEST_FONT}")
    endif()
    add_test(NAME graphics.rhi_composition COMMAND dy_renderer_composition_tests ${composition_args})
endif()
